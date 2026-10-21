#!/usr/bin/env python3
"""Hermes gadget v1 协议探针（P0 契约实测）。

在进程内启动生产版 DeviceHub（plugin/hub.py）+ devserver 的 EchoBrain，
用一个真实 WebSocket 客户端把设备固件将要走的每条路径全部走一遍，
并把收发的每一帧记录成 hermes_contract.json：

  场景 A（require_pairing=False）
    A1 新设备注册   hello -> challenge(enrolled=false) -> auth{key} -> welcome(paired=true)
    A2 文本轮次     text -> turn.start/status/reply.delta(累计)/reply/turn.end
    A3 语音轮次     audio.start + 二进制帧(channel 0x01) + audio.end -> 回复含时长
    A4 超短语音     0.1 s -> notice("Didn't catch that")
    A5 流中打断     cancel -> turn.end(cancelled)
    A6 ping/pong
  场景 B（require_pairing=True，另一个 state 目录 + 新设备）
    B1 未配对       welcome(paired=false) -> pairing{code} -> (approve) -> paired
    B2 重连 HMAC    challenge(enrolled=true) -> auth{mac}
    B3 错误密钥     错 mac -> error(auth_failed) + 连接关闭
"""

from __future__ import annotations

import asyncio
import base64
import json
import math
import struct
import sys
import tempfile
import time
from pathlib import Path

import websockets

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "hermes-gadget-sdk-main"))

from hermes_gadget.devserver import EchoBrain            # noqa: E402
from hermes_gadget_plugin import protocol               # noqa: E402
from hermes_gadget_plugin.hub import DeviceHub, DeviceStore  # noqa: E402

OUT = Path(__file__).resolve().parent / "hermes_contract.json"
HOST, PORT, PATH = "127.0.0.1", 18765, "/gadget"
URL = f"ws://{HOST}:{PORT}{PATH}"
LOG: list[dict] = []
T0 = time.monotonic()


def log(dirn: str, kind: str, size: int, note: str = "", data=None) -> None:
    rec = {"t": round(time.monotonic() - T0, 3), "dir": dirn, "kind": kind,
           "size": size, "note": note}
    if data is not None:
        rec["data"] = data
    LOG.append(rec)
    preview = json.dumps(data, ensure_ascii=False)[:150] if data is not None else note
    print(f"[{rec['t']:7.3f}] {dirn:>2} {kind:<14} {size:>5}B  {preview}")


def tone_pcm(rate: int, seconds: float, freq: float = 440.0) -> bytes:
    n = int(rate * seconds)
    return struct.pack("<%dh" % n, *(int(12000 * math.sin(2 * math.pi * freq * i / rate)) for i in range(n)))


def make_key(seed: int) -> bytes:
    return bytes((seed * 31 + i) & 0xFF for i in range(32))


class Client:
    """极简设备侧客户端：只做固件将要做的那些事。"""

    def __init__(self, key: bytes, name: str, token: str | None = None):
        self.key = key
        self.device_id = protocol.device_id_for_key(key)
        self.name = name
        self.token = token
        self.ws = None
        self.stream = 0

    async def __aenter__(self):
        self.ws = await websockets.connect(URL, subprotocols=[protocol.SUBPROTOCOL])
        return self

    async def __aexit__(self, *exc):
        await self.ws.close()

    async def send(self, msg: dict, note: str = "") -> None:
        raw = json.dumps(msg, ensure_ascii=False)
        await self.ws.send(raw)
        log("->", msg.get("type", "?"), len(raw.encode()), note, msg)

    async def send_audio_frame(self, seq: int, pcm: bytes) -> None:
        raw = protocol.binary(protocol.CHANNEL_AUDIO, self.stream, seq, pcm)
        await self.ws.send(raw)
        log("->", "binary[audio]", len(raw), f"seq={seq} pcm={len(pcm)}B")

    async def recv(self, timeout: float = 10.0):
        raw = await asyncio.wait_for(self.ws.recv(), timeout)
        if isinstance(raw, bytes):
            log("<-", "binary", len(raw), f"channel=0x{raw[0]:02x}")
            return {"type": "binary", "raw": raw}
        msg = json.loads(raw)
        log("<-", msg.get("type", "?"), len(raw.encode()), "", msg)
        return msg

    async def recv_until(self, want_type: str, timeout: float = 20.0) -> dict:
        while True:
            msg = await self.recv(timeout)
            if msg.get("type") == want_type:
                return msg

    async def handshake(self) -> dict:
        hello = {"type": "hello", "proto": 1, "device_id": self.device_id, "name": self.name,
                 "board": "esp-mosaico", "firmware": "probe",
                 "caps": {"display": {"width": 480, "height": 480, "color": True, "charset": "utf-8",
                                      "text_cols": 22, "text_rows": 10},
                          "mic": {"rate": 16000, "format": "pcm16"},
                          "inputs": ["talk"], "talk_mode": "hold"}}
        if self.token:
            hello["token"] = self.token
        await self.send(hello)
        challenge = await self.recv()
        assert challenge["type"] == "challenge", challenge
        if challenge["enrolled"]:
            mac = protocol.auth_mac(self.key, self.device_id, challenge["nonce"])
            await self.send({"type": "auth", "mac": mac})
        else:
            await self.send({"type": "auth", "key": base64.b64encode(self.key).decode()})
        welcome = await self.recv()
        assert welcome["type"] == "welcome", welcome
        return welcome

    async def utterance(self, pcm: bytes, rate: int = 16000) -> None:
        self.stream = (self.stream + 1) & 0xFF
        await self.send({"type": "audio.start", "id": f"a{self.stream}", "stream": self.stream,
                         "rate": rate, "format": "pcm16", "mode": "hold"})
        for seq in range(0, len(pcm), 512):
            await self.send_audio_frame(seq // 512, pcm[seq:seq + 512])
        await self.send({"type": "audio.end", "id": f"a{self.stream}", "stream": self.stream,
                         "duration_ms": len(pcm) * 1000 // (rate * 2)})


async def scenario_a() -> None:
    print("\n===== 场景 A：免配对全链路 =====")
    state = Path(tempfile.mkdtemp(prefix="hg-probe-a-"))
    brain = EchoBrain(require_pairing=False, loopback=False)
    hub = DeviceHub(DeviceStore(state), brain, host=HOST, port=PORT, path=PATH)
    brain.hub = hub
    await hub.start()
    try:
        async with Client(make_key(1), "Mosaico Probe A") as c:
            welcome = await c.handshake()
            log("==", "welcome", 0, f"paired={welcome['paired']} heartbeat_s={welcome['heartbeat_s']}")

            # A2 文本轮次（中文 + Markdown，验证 utf-8 charset 不折叠、Markdown 被剥离）
            await c.send({"type": "text", "id": "t1", "text": "**你好** Hermes"})
            await c.recv_until("turn.end")

            # A3 语音轮次 1.0 s
            await c.utterance(tone_pcm(16000, 1.0))
            await c.recv_until("turn.end")

            # A4 超短语音 0.1 s -> notice
            await c.utterance(tone_pcm(16000, 0.1))
            notice = await c.recv_until("notice")
            assert "catch" in notice["text"], notice

            # A5 流中打断
            await c.send({"type": "text", "id": "t2", "text": "long reply please"})
            await c.recv_until("reply.delta")
            await c.send({"type": "cancel"}, "打断")
            await c.recv_until("turn.end")

            # A6 ping/pong
            await c.send({"type": "ping", "ts": 12345})
            await c.recv_until("pong")

        # A7 同一设备重连走 HMAC 路径
        async with Client(make_key(1), "Mosaico Probe A") as c:
            welcome = await c.handshake()
            assert welcome["paired"] is True
            log("==", "re-hmac", 0, "enrolled reconnect OK")
    finally:
        await hub.stop()


async def scenario_b() -> None:
    print("\n===== 场景 B：配对 + 错误路径 =====")
    state = Path(tempfile.mkdtemp(prefix="hg-probe-b-"))
    brain = EchoBrain(require_pairing=True, loopback=False)
    hub = DeviceHub(DeviceStore(state), brain, host=HOST, port=PORT + 1, path=PATH)
    brain.hub = hub
    await hub.start()
    global URL
    url_a, URL = URL, f"ws://{HOST}:{PORT + 1}{PATH}"
    try:
        # B1 未配对 -> pairing -> approve -> paired
        async with Client(make_key(2), "Mosaico Probe B") as c:
            welcome = await c.handshake()
            assert welcome["paired"] is False, welcome
            pairing = await c.recv_until("pairing")
            code = pairing["code"]
            log("==", "pairing", 0, f"code={code} command={pairing['command']}")
            await brain.approve(code)
            await c.recv_until("paired")
            await c.send({"type": "text", "id": "t1", "text": "paired hello"})
            await c.recv_until("turn.end")

        # B2 重连 HMAC（已批准设备）
        async with Client(make_key(2), "Mosaico Probe B") as c:
            welcome = await c.handshake()
            assert welcome["paired"] is True
            log("==", "re-paired", 0, "HMAC 重连后仍 paired")

        # B3 错误密钥 -> error(auth_failed) + 关闭
        async with Client(make_key(3), "Mosaico Probe B") as c:
            c.key = make_key(2) + b"\x00"  # device_id 不匹配的废 key
            await c.send({"type": "hello", "proto": 1, "device_id": protocol.device_id_for_key(make_key(2)),
                          "name": "evil", "board": "x", "firmware": "0"})
            await c.recv()  # challenge
            await c.send({"type": "auth", "mac": "AAAAbbbbCCCCdddd=="})
            err = await c.recv()
            assert err["type"] == "error" and err["code"] == "auth_failed", err
            log("==", "auth_failed", 0, f"code={err['code']}")
    finally:
        URL = url_a
        await hub.stop()


async def main() -> None:
    await scenario_a()
    await scenario_b()
    OUT.write_text(json.dumps(LOG, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"\n共 {len(LOG)} 帧记录 -> {OUT}")


if __name__ == "__main__":
    asyncio.run(main())
