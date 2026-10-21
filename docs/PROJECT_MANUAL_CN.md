# Hermes Mosaico · 项目说明书

> 面向二次开发/维护者的技术说明，描述**当前代码的真实形态**（Hermes gadget 后端、纯屏显、流式上传）。由 [Octop 版说明书](https://github.com/samyeei/Octop-muse-esp-mosaico)改写而来，换后端的设计取舍见根目录 README 的架构章节。标注 `文件:行号` 可直接跳转核对。

## 1. 一句话与职责边界

设备 = **麦克风 + 屏幕 + 一条 WebSocket**。STT、大模型、Markdown 清理、配对授权全部在 Hermes 主机（[NousResearch/hermes-agent](https://github.com/NousResearch/hermes-agent) + [gadget 插件](https://github.com/Adolanium/hermes-gadget-sdk)）完成。设备只做：录音 → 流式上传 → 收 `transcript`/`reply.delta` 上屏 → 翻页阅读。

协议契约的权威记录：`projects/hermes_companion/tools/hermes_contract.json`（由 `tools/hermes_probe.py` 对生产版 hub 实测生成），协议文档为 SDK 的 `docs/protocol.md`。

## 2. 代码地图

```
projects/hermes_companion/
├── main/
│   ├── main.c                    # app_main：NVS → Recovery RPC → 配网判定 → UI → runtime
│   ├── companion_runtime.c       # 3 任务 + 3 队列运行时（mic/voice 在这里，不是组件）
│   ├── companion_model.c         # 纯函数 UI 状态机（PC 可单测，不知道后端存在）
│   ├── companion_ui.c            # GSP 渲染胶水：场景/姿态/翻页/双语/错误文案表
│   ├── companion_setup.c         # AP 热点 + 192.168.4.1 配网页（esp_http_server）
│   ├── companion_generation.c    # Iris RPC "文字生成"服务（走 hermes 文字轮次）
│   ├── companion_pager.h         # 阅读页像素预算分页算法（header-only）
│   ├── companion_platform.h      # UI 与后端之间唯一接缝
│   └── board_display.c           # CO5300 QSPI 屏 + 触摸 BSP 封装
├── components/
│   ├── companion_credentials/    # NVS 凭据 v3：{server_url(ws://…), access_token?}
│   └── hermes_link/              # 后端链路（本轮全部重写的部分）
│       ├── hermes_link.c         # 会话任务：握手/配对/心跳/流式上传/轮次/重连
│       ├── hermes_ws.c           # esp_websocket_client 封装：子协议/大帧重组/二进制发送
│       ├── hermes_wifi.c         # Wi-Fi STA 与 AP+STA 双模式（沿用 Octop 版实现）
│       ├── hermes_wire.c         # 纯逻辑：ws URL 拆分、JSON 抽取、JSON 转义（宿主可测）
│       └── hermes_crypto.c       # 自包含 SHA-256/HMAC/Base64（向量与主机侧钉死）
├── ui/                           # GSP 场景源（main/settings/read JSON）+ 字体 + 资产
├── pc/platform_pc.c              # PC 模拟器 mock 平台（HERMES_SIM_* 环境变量）
└── tools/                        # hermes_probe.py、prepare_ui.py、tests/ 等
```

`vendor/` 是退役的 Muse Gadget SDK 参考，**不在构建图内**（`dependencies.lock` 无条目、CMake 未引用）。

## 3. hermes_link：一条 WebSocket 上的全部生命

### 3.1 连接与身份

- 首启生成 32 字节随机密钥，NVS `companion_keys/device_key`；`device_id = "hg-" + sha256(key)[:16]`（`hermes_link.c` 的 `key_load/key_create`，SHA-256 在 `hermes_crypto.c`）。
- 握手（会话任务串行执行，全部发送都出自这一个任务）：
  `hello`（caps：480×480 utf-8、mic 16k pcm16、inputs talk、hold；无 speaker/ota/dashboard）→ `challenge{nonce,enrolled}` → `auth{key 首联 | mac 后续}`（`mac = base64(HMAC-SHA256(key, "hermes-gadget/v1|id|nonce"))`）→ `welcome{paired,heartbeat_s}`。
- 未配对 → 屏显配对码（`pairing` 帧 → `HERMES_PAIRING` 态，状态行拼「配对码 %s 等待主机批准」）；主机批准 → `paired` 帧 → `HERMES_ONLINE`。配对被撤销 → `unpaired` → 回 PAIRING。
- 心跳：服务端每 `heartbeat_s`（默认 20 s）发 `ping`，设备由会话任务回 `pong`；设备侧 3× 周期 + 15 s 无任何入帧即主动断线重连。重连退避 3 s 失败 / 12 s 成功（沿用 Wi-Fi 的节奏）。

### 3.2 语音轮次（与 Octop 版最大的行为差异）

| | Octop 版 | 本版 |
|---|---|---|
| 上传时机 | 松手后整段 WAV POST | **按住期间流式**：`audio.start` → 二进制帧（channel 0x01 + stream + seq，512–1024 B/帧）→ 松手 `audio.end` |
| 识别 | 设备调 `/api/voice/stt` | 主机 STT，`transcript` 帧回传 |
| 回复流 | `token` 增量帧 | `reply.delta` **累计文本**，link 层整包替换 reply 缓冲（UI 的「增量追加」消费方式不变，因为 runtime 每次都是整段 copy） |
| 打断 | `cancel` 带 thread_id | `{"type":"cancel"}`（Hermes `/stop`），`turn.end{cancelled}` 收尾 |

断线容错：录音期间掉线，PSRAM 30 s 裁剪区继续累积；链路恢复后会话任务以**新 stream 从头重放整段**（`sent_len` 清零），再继续实时发送——这是 `clip` 同时作为"实时缓冲 + 重放源"的原因。松手时若仍未连上，`JOB_VOICE_END` 内再尝试一次完整 bring-up，失败按 `turn_error()` 给出屏上原因。

短按丢弃：<0.25 s（8000 B）在设备端拦截（"没有听到声音，请按住再说"），已开流则补 `audio.cancel`，避免吃到服务端的 notice。

### 3.3 状态机与错误出口

`HERMES_NOT_SET / OFFLINE / CONNECTING / PAIRING / ONLINE / UNREACHABLE`，`hermes_link_status()` 附加 48 字节 detail（中文，与 `companion_ui.c` 的英文文案表一一对应）。轮次事件 `HERMES_EV_HEARD/REPLY/DONE/ERROR` 与 Octop 版同形，`companion_runtime.c` 只做了符号改名。

看门狗：`audio.end` 后 45 s 无任何帧（覆盖 STT+首字）、流中 60 s 无新帧 → 发 cancel 并以可读错误收尾；UI 外层 120 s 兜底不变。世代号 `generation` 机制原样保留——打断后迟到的帧整批丢弃。

### 3.4 未实现的协议面（有意裁剪）

设备未声明 `speaker`（主机不下发音频）、`ota`（更新走 mosaico 的 `iris system-update`）、`dashboard`/`image`/`actions`。`prompt` 问答到达时按"未知类型忽略"处理（协议允许），一期不做是/否按键映射——这是二期清单的第一项。主机侧 Markdown 清理依赖 `caps.display.charset:"utf-8"`（非 ascii 不折叠，中文原文直下，已在 probe 实测确认）。

## 4. 任务与队列拓扑（不变的部分）

```
companion_mic（4096, prio5, PSRAM 栈）──audio×48──▶ companion_voice（8192, prio4）
companion_voice ──control×12──▶ hermes_link（8192, prio5）──messages×2──▶ GSP 渲染任务
```

- `companion_mic`：`esp_codec_dev_read` 256 帧，录音窗口内投 audio 队列。
- `companion_voice`：唯一调用 `hermes_turn_*` 的任务；drain_audio 把帧交给 link（link 内部进 PSRAM 裁剪区并由会话任务流式发出）。
- `hermes_link`：会话任务（见 §3）。
- GSP 渲染任务：`companion_model` 状态机 + 100 ms tick 抽 messages 队列。

`companion_model.c` 的关键不变量：`COMP_MSG_REPLY` 可以从**任何非 LISTENING 态**进入接收态——所以主机不发 `transcript`（例如 devserver）时 UI 也不会卡在"识别中"，首个 `reply.delta` 直接触发。

## 5. 凭据与配网

- `companion_credentials` v3：`{server_url[256], access_token[128]}`，NVS `companion_keys/tokens`；v2（Octop 账号）记录整块作废，Wi-Fi 在独立的 `companion_wifi` 命名空间不受影响。URL 校验只放行 `ws://`/`wss://`、禁空格/`@`/`#`。
- 配网页（`setup.html`）字段：Wi-Fi 扫描/密码 + server_url + 可选 access_token；保存前真实试连 15 s。设备密钥**不进网页**——首联自动注册。
- 表单与固件的校验规则必须保持一致（`companion_credentials_valid` ↔ setup.html 的 `invalid` 分支），改一边必须改另一边，`tools/tests/setup_tests.c` 两侧都钉。

## 6. 构建与烧录

- 构建：`projects/hermes_companion/build_hermes.ps1`（包装 `idf_low_noise_build.py` → idf.py；IDF 6.2，target esp32s31，pin 见 `DEPENDENCIES.json`）。**只能 PowerShell**；Git Bash 里 `MSYSTEM` 会泄给 idf_tools.py 导致拒绝工作（`env -u` 拦不住 harness 注入，PowerShell `Remove-Item Env:MSYSTEM` 可靠）。
- 产物：`hermes_companion.bin`（约 9.97 MB，含字体）+ `ui_apps.bin`（GSP bundle）+ `*-system-update.irisfw`（整包，`mosaico.py iris system-update` 生成并上机）。`ota_0` 余量约 1.44 MB。
- 每次构建前脚本自动删 `build/.ninja_deps`（GSP 生成文件路径问题）。
- 宿主测试：`tools/tests/`（credentials v3 + ws URL + JSON 帧 + 加密向量）与 `pc/companion_model_test.c`（状态机），命令见 [BUILDING.md](BUILDING.md)。
- 协议探针：`tools/hermes_probe.py`（需要 `pip install -e <hermes-gadget-sdk>`，纯 websockets 依赖）。

## 7. 二期清单（协议已留位）

prompt 触屏是/否问答（TALK=yes 映射） · dashboard 状态瓦片（caps.dashboard + 设置页） · 图片 RGB565 下行（480×480 屏很适合展示） · 电池传感器上报（BQ27220 → hello.sensors + state 帧） · `session.new` 长按手势（Hermes `/new`）。

## 8. 限制（当前真实形态）

- 无喇叭、无 TTS、无 BLE、无 OTA-over-protocol；屏显是唯一输出。
- 语音依赖主机 STT 配置；未配置时语音轮次表现为超时错误（文字轮次不受影响）。
- 明文 `ws://` 在不可信网络会暴露对话内容（密钥在首联后不再明文出现）；需要时主机配 `tls_cert/tls_key`，设备配网页直接支持 `wss://`。
- 30 s 录音上限；64 KiB 回复上限（超出截断加省略号）；每页约 220 汉字。
- 真机 + 真 Hermes 主机的端到端验收待主机到位后补做（30 轮回归）；开发期以 devserver + 模拟器为验收口径。
