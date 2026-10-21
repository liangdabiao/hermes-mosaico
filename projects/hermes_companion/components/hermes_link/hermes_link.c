// SPDX-License-Identifier: Apache-2.0
// The Hermes gadget session: keep Wi-Fi, the device key and one WebSocket to
// the gateway plugin up, then run push-to-talk turns in order. Everything
// blocking (connect, handshake) and everything that emits a frame happens on
// this task; the app appends audio into the PSRAM clip and polls events.
//
// Turn shape (measured against the production hub, tools/hermes_contract.json):
//   press   : audio.start, then binary channel-1 frames follow the clip
//   release : audio.end -> transcript -> turn.start -> reply.delta (the text
//             is CUMULATIVE) xN -> reply -> turn.end{outcome}
//   cancel  : {"type":"cancel"} -> turn.end{cancelled} ~instantly
// A drop mid-recording is survivable: the clip keeps growing in PSRAM and the
// next successful bring-up replays it from the start as a fresh stream.
#include "hermes_link.h"
#include "hermes_internal.h"
#include "hermes_crypto.h"
#include "hermes_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "companion_credentials.h"

static const char *TAG = "hermes_link";
enum {
    /* 30 s at 16 kHz mono 16-bit is 960 kB of PCM: this board has PSRAM and no
     * room for that in internal RAM, so the clip lives there. The clip doubles
     * as the replay buffer when the socket drops mid-recording. */
    CLIP_MAX = 30 * 16000 * 2,
    /* The hub drops utterances under 0.25 s with a notice; catching it here
     * keeps the reason on the turn instead of a stray notice. */
    CLIP_MIN = 8000,
    REPLY_CAP = 65536,
    /* Transcript and error lines shown on the card; long by error standards
     * because the transcript is a user-visible sentence. */
    EVENT_CAP = 256,
    /* First frame after audio.end covers host STT plus the agent's first
     * token; the live Octop needed up to 13 s for a first token, and Hermes
     * hosts run arbitrary models, so the same generous windows stay. */
    FIRST_FRAME_WAIT_US = 45000000LL,
    TOKEN_IDLE_US = 60000000LL,
    BRING_UP_PERIOD_MS = 3000,
    HANDSHAKE_FRAME_WAIT_MS = 10000,
    /* Send at most this much new clip per loop turn; the loop runs every
     * ~100 ms, so 2 kB keeps 16 kB/s real time ahead with headroom. */
    STREAM_CHUNK = 1024,
};

typedef enum { JOB_VOICE_END, JOB_TEXT, JOB_CANCEL } job_kind_t;
typedef struct { job_kind_t kind; char *text; } link_job_t;
typedef struct { hermes_ev_t kind; char text[EVENT_CAP]; } link_event_t;
typedef enum { TURN_IDLE, TURN_CHAT } turn_state_t;

struct link_t {
    QueueHandle_t jobs, events;
    SemaphoreHandle_t lock;
    int16_t *clip;
    char *reply;
    /* clip/reply are written by the voice task and read by this task. */
    size_t clip_len, reply_len;
    size_t sent_len;            /* session task only: clip bytes already sent */
    uint16_t seq;               /* session task only: next binary seq */
    uint8_t stream_id;          /* session task only: current audio stream */
    volatile bool recording, streaming, cancelled;
    hermes_state_t state;
    char detail[HERMES_DETAIL_CAP];
    char pairing_code[HERMES_CODE_CAP];
    /* Device identity: a random 32-byte key in NVS; the id derives from it. */
    uint8_t key[32];
    char device_id[20];
    /* Handshake mailbox: written by the frame classifier on the ws task,
     * consumed by this task between hello and welcome. */
    bool hs_challenge, hs_welcome, hs_failed, hs_enrolled;
    char hs_nonce[HERMES_NONCE_CAP], hs_error[24];
    bool hs_paired;
    int heartbeat_s;
    volatile bool pong_pending;
    char pong_ts[24];
    /* Turn bookkeeping. */
    turn_state_t turn;
    bool turn_is_text;
    unsigned text_seen_len;     /* observer deltas need the suffix, not the sum */
    int64_t last_frame_us, next_bring_up_us, last_rx_us;
    bool saw_reply;
    hermes_ws_handlers_t handlers;
    hermes_text_cb observer;
    void *observer_ctx;
};
static struct link_t link;

/* ---------------- small helpers ---------------- */

static void set_state(struct link_t *l, hermes_state_t state, const char *detail)
{
    l->state = state;
    hermes_copy_text(l->detail, sizeof(l->detail), detail ? detail : "");
}

const char *hermes_state_name(hermes_state_t state)
{
    switch (state) {
    case HERMES_NOT_SET: return "not-set";
    case HERMES_OFFLINE: return "offline";
    case HERMES_CONNECTING: return "connecting";
    case HERMES_PAIRING: return "pairing";
    case HERMES_ONLINE: return "online";
    default: return "unreachable";
    }
}

const char *hermes_pairing_code(void)
{
    static char code[HERMES_CODE_CAP];
    xSemaphoreTake(link.lock, portMAX_DELAY);
    hermes_copy_text(code, sizeof(code), link.pairing_code);
    xSemaphoreGive(link.lock);
    return link.state == HERMES_PAIRING ? code : "";
}

static void push_event(struct link_t *l, hermes_ev_t kind, const char *text)
{
    link_event_t e = {.kind = kind};
    hermes_copy_text(e.text, sizeof(e.text), text ? text : "");
    if (xQueueSend(l->events, &e, 0) != pdTRUE)
        ESP_LOGW(TAG, "event queue full");
}

static volatile bool reply_pending;
static void push_reply(void)
{
    if (reply_pending) return;
    link_event_t e = {.kind = HERMES_EV_REPLY};
    if (xQueueSend(link.events, &e, 0) == pdTRUE) reply_pending = true;
    else ESP_LOGW(TAG, "event queue full");
}

static void notify_text(struct link_t *l, const char *event, const char *text)
{
    if (l->observer) l->observer(l->observer_ctx, event, text);
}

void hermes_text_observer(hermes_text_cb cb, void *ctx)
{
    link.observer = cb;
    link.observer_ctx = ctx;
}

void hermes_language(unsigned language) { (void)language; }

/* ---------------- device identity ---------------- */

static bool key_load(struct link_t *l)
{
    nvs_handle_t h;
    if (nvs_open("companion_keys", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof(l->key);
    esp_err_t err = nvs_get_blob(h, "device_key", l->key, &n);
    nvs_close(h);
    return err == ESP_OK && n == sizeof(l->key);
}

static void key_create(struct link_t *l)
{
    esp_fill_random(l->key, sizeof(l->key));
    nvs_handle_t h;
    if (nvs_open("companion_keys", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "device_key", l->key, sizeof(l->key)) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
}

/* ---------------- frame classifier (ws task) ---------------- */

/* Reply text arrives cumulative: replace the buffer wholesale, which is also
 * what a resent or restarted stream wants. `delta` additionally feeds the
 * text-channel observer with just the new suffix. */
static void take_reply(struct link_t *l, const char *text, bool delta)
{
    xSemaphoreTake(l->lock, portMAX_DELAY);
    size_t n = strnlen(text, REPLY_CAP - 4);
    bool clipped = strlen(text) > n;
    memcpy(l->reply, text, n);
    if (clipped && n + 3 < REPLY_CAP) { memcpy(l->reply + n, "…", 3); n += 3; }
    l->reply_len = n;
    l->reply[n] = '\0';
    unsigned now_len = (unsigned)n;
    xSemaphoreGive(l->lock);
    if (delta && l->turn_is_text) {
        if (now_len > l->text_seen_len)
            notify_text(l, "delta", text + l->text_seen_len);
        else if (now_len < l->text_seen_len)
            notify_text(l, "delta", text);   /* the stream restarted */
        l->text_seen_len = now_len;
    }
}

static void finish_turn(struct link_t *l, const char *error);

static void on_frame(struct link_t *l, const char *frame)
{
    char type[24], text[EVENT_CAP];
    if (!hermes_json_string(frame, "type", type, sizeof(type))) return;
    bool has_text = hermes_json_string(frame, "text", text, sizeof(text));

    if (!strcmp(type, "challenge")) {
        xSemaphoreTake(l->lock, portMAX_DELAY);
        hermes_copy_text(l->hs_nonce, sizeof(l->hs_nonce), text);
        hermes_json_bool(frame, "enrolled", &l->hs_enrolled);
        l->hs_challenge = true;
        xSemaphoreGive(l->lock);
        return;
    }
    if (!strcmp(type, "welcome")) {
        xSemaphoreTake(l->lock, portMAX_DELAY);
        hermes_json_bool(frame, "paired", &l->hs_paired);
        long hb = 0;
        if (hermes_json_long(frame, "heartbeat_s", &hb) && hb >= 5 && hb <= 120)
            l->heartbeat_s = (int)hb;
        l->hs_welcome = true;
        xSemaphoreGive(l->lock);
        return;
    }
    if (!strcmp(type, "error")) {
        char code[24];
        hermes_json_string(frame, "code", code, sizeof(code));
        xSemaphoreTake(l->lock, portMAX_DELAY);
        hermes_copy_text(l->hs_error, sizeof(l->hs_error), code);
        l->hs_failed = true;
        xSemaphoreGive(l->lock);
        if (l->turn == TURN_CHAT)
            finish_turn(l, l->turn_is_text ? "server error" : "服务器拒绝了这次请求");
        return;
    }
    if (!strcmp(type, "pairing")) {
        char code[HERMES_CODE_CAP];
        if (hermes_json_string(frame, "code", code, sizeof(code))) {
            xSemaphoreTake(l->lock, portMAX_DELAY);
            hermes_copy_text(l->pairing_code, sizeof(l->pairing_code), code);
            if (l->state == HERMES_CONNECTING || l->state == HERMES_ONLINE)
                set_state(l, HERMES_PAIRING, "");
            xSemaphoreGive(l->lock);
        }
        return;
    }
    if (!strcmp(type, "paired")) {
        xSemaphoreTake(l->lock, portMAX_DELAY);
        l->pairing_code[0] = '\0';
        if (l->state == HERMES_PAIRING) set_state(l, HERMES_ONLINE, NULL);
        xSemaphoreGive(l->lock);
        ESP_LOGI(TAG, "paired by host");
        return;
    }
    if (!strcmp(type, "unpaired")) {
        xSemaphoreTake(l->lock, portMAX_DELAY);
        set_state(l, HERMES_PAIRING, "配对被撤销");
        xSemaphoreGive(l->lock);
        return;
    }
    if (!strcmp(type, "ping")) {
        xSemaphoreTake(l->lock, portMAX_DELAY);
        hermes_json_string(frame, "ts", l->pong_ts, sizeof(l->pong_ts));
        l->pong_ts[sizeof(l->pong_ts) - 1] = '\0';
        l->pong_pending = true;
        xSemaphoreGive(l->lock);
        return;
    }
    if (!strcmp(type, "transcript")) {
        l->last_frame_us = esp_timer_get_time();
        if (l->turn == TURN_CHAT && !l->cancelled && has_text && text[0])
            push_event(l, HERMES_EV_HEARD, text);
        return;
    }
    if (!strcmp(type, "turn.start") || !strcmp(type, "status")) {
        /* Any frame is proof of life; the phrases themselves stay off the
         * screen because the caption already says what is happening. */
        l->last_frame_us = esp_timer_get_time();
        return;
    }
    if (!strcmp(type, "reply.delta")) {
        if (l->turn != TURN_CHAT || l->cancelled || !has_text || !text[0]) return;
        l->last_frame_us = esp_timer_get_time();
        if (!l->saw_reply) {
            l->saw_reply = true;
            if (l->turn_is_text) { l->text_seen_len = 0; notify_text(l, "start", ""); }
        }
        take_reply(l, text, true);
        if (!l->turn_is_text) push_reply();
        return;
    }
    if (!strcmp(type, "reply")) {
        if (l->turn == TURN_CHAT && !l->cancelled && has_text)
            take_reply(l, text, true);
        return;
    }
    if (!strcmp(type, "turn.end")) {
        char outcome[16];
        hermes_json_string(frame, "outcome", outcome, sizeof(outcome));
        if (l->turn != TURN_CHAT) return;
        l->last_frame_us = esp_timer_get_time();
        if (!strcmp(outcome, "failure")) {
            xSemaphoreTake(l->lock, portMAX_DELAY);
            bool any = l->reply_len > 0;
            xSemaphoreGive(l->lock);
            /* A half-arrived answer beats an error line. */
            finish_turn(l, any ? NULL : (l->turn_is_text ? "turn failed" : "回答失败，请重试"));
        } else finish_turn(l, NULL);
        return;
    }
    if (!strcmp(type, "notice")) {
        if (has_text && text[0])
            push_event(l, HERMES_EV_ERROR, text[0] ? text : "提示");
        return;
    }
    /* prompt / display / image.* / dashboard / ota.*: not advertised and not
     * drawn in this firmware; the protocol says unknown types are ignored. */
}

static void on_ws_text(void *ctx, const char *frame) { on_frame(ctx, frame); }

static void on_ws_lost(void *ctx, int handshake_status)
{
    struct link_t *l = ctx;
    ESP_LOGW(TAG, "link lost (handshake %d)", handshake_status);
    xSemaphoreTake(l->lock, portMAX_DELAY);
    if (l->state == HERMES_ONLINE || l->state == HERMES_PAIRING ||
        l->state == HERMES_CONNECTING)
        set_state(l, HERMES_UNREACHABLE, "连接中断，正在重连");
    xSemaphoreGive(l->lock);
    l->streaming = false;
    if (l->turn == TURN_CHAT) {
        bool was_text = l->turn_is_text;
        l->turn = TURN_IDLE;
        l->saw_reply = false;
        if (l->cancelled) l->cancelled = false;
        else if (was_text) notify_text(l, "error", "connection lost");
        else push_event(l, HERMES_EV_ERROR, "与 Hermes 的连接中断");
    }
    /* Recording survives a drop: the clip keeps filling, and the next
     * bring-up replays it from byte zero as a fresh stream. */
}

/* ---------------- handshake (session task) ---------------- */

static bool send_json(struct link_t *l, const char *frame)
{
    size_t n = strlen(frame);
    if (hermes_ws_send_text(frame, n)) return true;
    ESP_LOGW(TAG, "send failed: %.40s", frame);
    return false;
}

/* hello -> challenge -> auth -> welcome. Runs on the session task so every
 * device frame is emitted from one task. */
static bool handshake(struct link_t *l)
{
    const char *token = companion_credentials_access_token();
    char escaped[COMPANION_TOKEN_CAP];
    escaped[0] = '\0';
    if (token && token[0] && !hermes_json_escape(token, escaped, sizeof(escaped))) {
        set_state(l, HERMES_UNREACHABLE, "访问令牌太长");
        return false;
    }
    /* One of two fixed shapes; the token slot is dropped entirely when the
     * host runs without GADGET_ACCESS_TOKEN. */
    char hello[640];
    int n = escaped[0]
        ? snprintf(hello, sizeof(hello),
            "{\"type\":\"hello\",\"proto\":1,\"device_id\":\"%s\",\"name\":\"Mosaico\","
            "\"board\":\"esp-mosaico\",\"firmware\":\"1.0.0-hermes\",\"token\":\"%s\","
            "\"caps\":{\"display\":{\"width\":480,\"height\":480,\"color\":true,"
            "\"charset\":\"utf-8\",\"text_cols\":22,\"text_rows\":10},"
            "\"mic\":{\"rate\":16000,\"format\":\"pcm16\"},"
            "\"inputs\":[\"talk\"],\"talk_mode\":\"hold\"}}",
            l->device_id, escaped)
        : snprintf(hello, sizeof(hello),
            "{\"type\":\"hello\",\"proto\":1,\"device_id\":\"%s\",\"name\":\"Mosaico\","
            "\"board\":\"esp-mosaico\",\"firmware\":\"1.0.0-hermes\","
            "\"caps\":{\"display\":{\"width\":480,\"height\":480,\"color\":true,"
            "\"charset\":\"utf-8\",\"text_cols\":22,\"text_rows\":10},"
            "\"mic\":{\"rate\":16000,\"format\":\"pcm16\"},"
            "\"inputs\":[\"talk\"],\"talk_mode\":\"hold\"}}",
            l->device_id);
    if (n < 0 || (size_t)n >= sizeof(hello)) return false;

    xSemaphoreTake(l->lock, portMAX_DELAY);
    l->hs_challenge = l->hs_welcome = l->hs_failed = false;
    xSemaphoreGive(l->lock);
    if (!send_json(l, hello)) {
        set_state(l, HERMES_UNREACHABLE, "无法发送握手");
        return false;
    }
    for (unsigned waited = 0; waited < HANDSHAKE_FRAME_WAIT_MS; waited += 50) {
        if (l->hs_challenge) break;
        if (l->hs_failed) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    char nonce[HERMES_NONCE_CAP];
    bool enrolled = false;
    xSemaphoreTake(l->lock, portMAX_DELAY);
    hermes_copy_text(nonce, sizeof(nonce), l->hs_nonce);
    enrolled = l->hs_enrolled;
    bool failed = l->hs_failed;
    char code[24];
    hermes_copy_text(code, sizeof(code), l->hs_error);
    xSemaphoreGive(l->lock);
    if (failed) {
        set_state(l, HERMES_UNREACHABLE,
                  !strcmp(code, "bad_token") ? "访问令牌不对" : "握手被拒绝");
        return false;
    }
    if (!link.hs_challenge) {
        set_state(l, HERMES_UNREACHABLE, "握手超时");
        return false;
    }
    char auth[160];
    if (enrolled) {
        /* mac = base64(HMAC-SHA256(key, "hermes-gadget/v1|id|nonce")) */
        uint8_t mac[32];
        char msg[160];
        int m = snprintf(msg, sizeof(msg), "hermes-gadget/v1|%s|%s",
                         l->device_id, nonce);
        hermes_hmac_sha256(l->key, sizeof(l->key), (const uint8_t *)msg, (size_t)m, mac);
        char b64[HERMES_MAC_CAP];
        hermes_base64_encode(mac, sizeof(mac), b64, sizeof(b64));
        snprintf(auth, sizeof(auth), "{\"type\":\"auth\",\"mac\":\"%s\"}", b64);
    } else {
        char b64[48];
        hermes_base64_encode(l->key, sizeof(l->key), b64, sizeof(b64));
        snprintf(auth, sizeof(auth), "{\"type\":\"auth\",\"key\":\"%s\"}", b64);
    }
    if (!send_json(l, auth)) {
        set_state(l, HERMES_UNREACHABLE, "无法发送认证");
        return false;
    }
    for (unsigned waited = 0; waited < HANDSHAKE_FRAME_WAIT_MS; waited += 50) {
        if (l->hs_welcome || l->hs_failed) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    bool paired = false;
    xSemaphoreTake(l->lock, portMAX_DELAY);
    paired = l->hs_paired;
    failed = l->hs_failed;
    hermes_copy_text(code, sizeof(code), l->hs_error);
    xSemaphoreGive(l->lock);
    if (failed) {
        if (!strcmp(code, "auth_failed"))
            set_state(l, HERMES_UNREACHABLE,
                      "密钥被拒绝，请在主机执行 hermes gadget forget");
        else set_state(l, HERMES_UNREACHABLE, "认证失败");
        return false;
    }
    if (!l->hs_welcome) {
        set_state(l, HERMES_UNREACHABLE, "等待 welcome 超时");
        return false;
    }
    set_state(l, paired ? HERMES_ONLINE : HERMES_PAIRING,
              paired ? NULL : "等待主机批准配对");
    ESP_LOGI(TAG, "%s as %s (heartbeat %ds)", paired ? "online" : "awaiting pairing",
             l->device_id, l->heartbeat_s);
    return true;
}

static bool bring_up(struct link_t *l)
{
    if (!companion_credentials_ready()) {
        set_state(l, HERMES_NOT_SET, NULL);
        return false;
    }
    if (!hermes_wifi_connected()) {
        set_state(l, HERMES_OFFLINE, NULL);
        return false;
    }
    if (hermes_ws_open()) return true;   /* PAIRING keeps its socket */
    hermes_url_t url;
    if (!hermes_url_split(companion_credentials_server_url(), &url)) {
        set_state(l, HERMES_UNREACHABLE, "地址无效");
        return false;
    }
    set_state(l, HERMES_CONNECTING, "正在连接");
    if (!hermes_ws_start(&url, &l->handlers)) {
        set_state(l, HERMES_UNREACHABLE, "连不上服务器");
        return false;
    }
    return handshake(l);
}

/* ---------------- utterance streaming ---------------- */

static bool send_audio_start(struct link_t *l)
{
    l->stream_id = (uint8_t)(l->stream_id + 1);
    l->seq = 0;
    l->sent_len = 0;
    char frame[128];
    snprintf(frame, sizeof(frame),
             "{\"type\":\"audio.start\",\"id\":\"a%u\",\"stream\":%u,"
             "\"rate\":16000,\"format\":\"pcm16\",\"mode\":\"hold\"}",
             (unsigned)l->stream_id, (unsigned)l->stream_id);
    if (!send_json(l, frame)) return false;
    l->streaming = true;
    return true;
}

/* Push unsent clip bytes out in channel-1 binary frames. Returns false when
 * the transport rejected the write; the caller clears `streaming` and the
 * next bring-up replays the clip from the start. */
static bool pump_clip(struct link_t *l)
{
    while (l->streaming) {
        uint8_t chunk[STREAM_CHUNK];
        size_t take = 0;
        xSemaphoreTake(l->lock, portMAX_DELAY);
        if (l->sent_len < l->clip_len) {
            take = l->clip_len - l->sent_len;
            if (take > STREAM_CHUNK) take = STREAM_CHUNK;
            memcpy(chunk, (const uint8_t *)l->clip + l->sent_len, take);
            l->sent_len += take;
        }
        xSemaphoreGive(l->lock);
        if (!take) return true;
        if (!hermes_ws_send_binary(0x01, l->stream_id, l->seq++, chunk, take)) {
            xSemaphoreTake(l->lock, portMAX_DELAY);
            l->sent_len -= take;   /* this chunk never reached the wire */
            xSemaphoreGive(l->lock);
            return false;
        }
    }
    return true;
}

static void finish_turn(struct link_t *l, const char *error)
{
    bool was_text = l->turn_is_text, silenced = l->cancelled;
    l->turn = TURN_IDLE;
    l->saw_reply = false;
    l->cancelled = false;
    if (silenced) {
        if (was_text) notify_text(l, "done", "");
        return;
    }
    if (error) {
        if (was_text) notify_text(l, "error", error);
        else push_event(l, HERMES_EV_ERROR, error);
        return;
    }
    if (was_text) notify_text(l, "done", "");
    else push_event(l, HERMES_EV_DONE, "");
}

static void start_chat_turn(struct link_t *l, bool from_text)
{
    xSemaphoreTake(l->lock, portMAX_DELAY);
    l->reply_len = 0;
    l->reply[0] = '\0';
    xSemaphoreGive(l->lock);
    reply_pending = false;
    l->turn_is_text = from_text;
    l->text_seen_len = 0;
    l->saw_reply = false;
    l->cancelled = false;
    l->turn = TURN_CHAT;
    l->last_frame_us = esp_timer_get_time();
}

static const char *turn_error(struct link_t *l)
{
    if (l->detail[0] && l->state != HERMES_PAIRING && l->state != HERMES_ONLINE)
        return l->detail;
    switch (l->state) {
    case HERMES_NOT_SET: return "还没有配置服务，请先用手机完成配置";
    case HERMES_OFFLINE: return "网络未连接，请稍后重试";
    case HERMES_PAIRING: return "还没配对：请把屏幕上的配对码告诉主机";
    default: return "Hermes 连不上，请检查配置";
    }
}

static void run_job(struct link_t *l, link_job_t *job)
{
    if (job->kind == JOB_CANCEL) {
        if (l->streaming && l->turn != TURN_CHAT) {
            /* The press turned into a bail-out while the utterance was open:
             * clear the server's upload before it ever reaches STT. */
            send_json(l, "{\"type\":\"audio.cancel\"}");
            l->streaming = false;
            return;
        }
        if (l->turn == TURN_CHAT) send_json(l, "{\"type\":\"cancel\"}");
        else finish_turn(l, NULL);
        return;
    }
    if (job->kind == JOB_TEXT) {
        if (!bring_up(l)) {
            notify_text(l, "error", turn_error(l));
            free(job->text);
            return;
        }
        if (l->state == HERMES_PAIRING) {
            notify_text(l, "error", "还没配对：请把屏幕上的配对码告诉主机");
            free(job->text);
            return;
        }
        /* {"type":"text","id":"g1","text":"<escaped>"}; prompts for character
         * generation reach tens of kilobytes, so the frame is heap-built. */
        static const char prefix[] = "{\"type\":\"text\",\"id\":\"g1\",\"text\":\"";
        size_t prefix_len = sizeof(prefix) - 1;
        size_t cap = prefix_len + strlen(job->text) * 2 + 8;
        char *frame = cap > 65536
            ? heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : malloc(cap);
        bool ok = frame;
        if (ok) {
            memcpy(frame, prefix, prefix_len);
            ok = hermes_json_escape(job->text, frame + prefix_len, cap - prefix_len - 3);
        }
        if (ok) {
            size_t end = prefix_len + strlen(frame + prefix_len);
            frame[end] = '"';
            frame[end + 1] = '}';
            frame[end + 2] = '\0';
            start_chat_turn(l, true);
            if (!send_json(l, frame))
                finish_turn(l, "send failed");
        } else finish_turn(l, "could not build request");
        free(job->text);
        free(frame);
        return;
    }
    /* JOB_VOICE_END: the button was released. */
    if (l->streaming) {
        if (!pump_clip(l)) {
            l->streaming = false;
            push_event(l, HERMES_EV_ERROR, "上传中断，请重试");
            return;
        }
    }    size_t clip_len;
    xSemaphoreTake(l->lock, portMAX_DELAY);
    clip_len = l->clip_len;
    xSemaphoreGive(l->lock);
    if (clip_len < CLIP_MIN) {
        if (l->streaming) {
            send_json(l, "{\"type\":\"audio.cancel\"}");
            l->streaming = false;
        }
        push_event(l, HERMES_EV_ERROR, "没有听到声音，请按住再说");
        return;
    }
    if (!l->streaming) {
        /* Never connected during the press (or the socket dropped): try once
         * more now; the whole clip replays as one utterance. */
        if (!bring_up(l) || l->state == HERMES_PAIRING) {
            push_event(l, HERMES_EV_ERROR, turn_error(l));
            return;
        }
        if (!send_audio_start(l) || !pump_clip(l)) {
            l->streaming = false;
            push_event(l, HERMES_EV_ERROR, "上传失败，请重试");
            return;
        }
    }
    char frame[96];
    snprintf(frame, sizeof(frame),
             "{\"type\":\"audio.end\",\"id\":\"a%u\",\"stream\":%u,\"duration_ms\":%u}",
             (unsigned)l->stream_id, (unsigned)l->stream_id,
             (unsigned)(clip_len / 32));
    if (!send_json(l, frame)) {
        push_event(l, HERMES_EV_ERROR, "上传失败，请重试");
        return;
    }
    l->streaming = false;   /* audio.end closes the utterance */
    start_chat_turn(l, false);
}

static void watch_turn(struct link_t *l)
{
    if (l->turn != TURN_CHAT) return;
    int64_t silence = l->saw_reply ? TOKEN_IDLE_US : FIRST_FRAME_WAIT_US;
    if (esp_timer_get_time() - l->last_frame_us > silence) {
        send_json(l, "{\"type\":\"cancel\"}");
        finish_turn(l, l->turn_is_text ? (l->saw_reply ? "stream stopped" : "no answer")
            : (l->saw_reply ? "回答中断，请重试" : "等了 45 秒没有回答，请重试"));
    }
}

static void keep_alive(struct link_t *l)
{
    if (!hermes_ws_open()) return;
    if (l->pong_pending) {
        char pong[64], ts[24];
        xSemaphoreTake(l->lock, portMAX_DELAY);
        hermes_copy_text(ts, sizeof(ts), l->pong_ts);
        l->pong_pending = false;
        xSemaphoreGive(l->lock);
        snprintf(pong, sizeof(pong), "{\"type\":\"pong\",\"ts\":%s}",
                 ts[0] ? ts : "0");
        send_json(l, pong);
    }
    int hb = l->heartbeat_s > 0 ? l->heartbeat_s : 20;
    if (esp_timer_get_time() - hermes_ws_last_rx_us() > (3 * hb + 15) * 1000000LL) {
        ESP_LOGW(TAG, "server silent for %ds s, dropping the link", 3 * hb + 15);
        hermes_ws_stop();   /* on_lost flips the state and the retry timer */
    }
}

static void link_task(void *arg)
{
    struct link_t *l = arg;
    bring_up(l);
    l->next_bring_up_us = esp_timer_get_time();
    for (;;) {
        link_job_t job;
        if (xQueueReceive(l->jobs, &job, pdMS_TO_TICKS(100)) == pdTRUE) run_job(l, &job);
        /* Streaming starts the moment the link is usable, not at release, so
         * the host hears the user live and STT begins at audio.end. */
        if (l->recording && !l->streaming && hermes_ws_open() &&
            l->state == HERMES_ONLINE) {
            if (!send_audio_start(l)) l->streaming = false;
        }
        if (l->streaming && !pump_clip(l)) l->streaming = false;
        watch_turn(l);
        keep_alive(l);
        if (!hermes_ws_open() && !l->recording &&
            esp_timer_get_time() > l->next_bring_up_us) {
            bool ok = bring_up(l);
            l->next_bring_up_us = esp_timer_get_time() +
                (ok ? BRING_UP_PERIOD_MS * 4 : BRING_UP_PERIOD_MS) * 1000LL;
        }
    }
}

/* ---------------- public surface ---------------- */

esp_err_t hermes_link_start(void)
{
    struct link_t *l = &link;
    l->handlers = (hermes_ws_handlers_t){
        .on_text = on_ws_text, .on_lost = on_ws_lost, .ctx = l};
    l->jobs = xQueueCreate(2, sizeof(link_job_t));
    l->events = xQueueCreate(6, sizeof(link_event_t));
    l->lock = xSemaphoreCreateMutex();
    l->clip = heap_caps_malloc(CLIP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    l->reply = heap_caps_calloc(1, REPLY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!l->jobs || !l->events || !l->lock || !l->clip || !l->reply)
        return ESP_ERR_NO_MEM;
    if (!key_load(l)) key_create(l);
    hermes_device_id(l->key, l->device_id);
    set_state(l, HERMES_CONNECTING, "启动中");
    if (hermes_wifi_start() != ESP_OK)
        ESP_LOGW(TAG, "no saved Wi-Fi network yet");
    if (xTaskCreate(link_task, "hermes_link", 8192, l, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool hermes_link_ready(void) { return link.state == HERMES_ONLINE; }

void hermes_link_status(hermes_status_t *out)
{
    if (!out) return;
    xSemaphoreTake(link.lock, portMAX_DELAY);
    out->state = link.state;
    if (out->state == HERMES_PAIRING && link.pairing_code[0]) {
        snprintf(out->detail, sizeof(out->detail), "配对码 %s 等待主机批准",
                 link.pairing_code);
    } else {
        hermes_copy_text(out->detail, sizeof(out->detail), link.detail);
    }
    xSemaphoreGive(link.lock);
}

bool hermes_turn_ready(void)
{
    return companion_credentials_ready() && hermes_wifi_connected();
}

void hermes_turn_begin(void)
{
    xSemaphoreTake(link.lock, portMAX_DELAY);
    link.clip_len = 0;
    link.sent_len = 0;
    xSemaphoreGive(link.lock);
    link.cancelled = false;
    link.recording = true;
    link.streaming = false;
}

void hermes_turn_audio(const int16_t *pcm, size_t frames)
{
    if (!link.recording || !pcm || !frames) return;
    size_t bytes = frames * sizeof(int16_t);
    xSemaphoreTake(link.lock, portMAX_DELAY);
    if (link.clip_len + bytes > CLIP_MAX) {
        link.recording = false;         /* the 30 s ceiling is reached */
        xSemaphoreGive(link.lock);
        return;
    }
    memcpy((uint8_t *)link.clip + link.clip_len, pcm, bytes);
    link.clip_len += bytes;
    xSemaphoreGive(link.lock);
}

void hermes_turn_end(void)
{
    link.recording = false;
    link_job_t job = {.kind = JOB_VOICE_END};
    if (xQueueSend(link.jobs, &job, pdMS_TO_TICKS(50)) != pdTRUE)
        ESP_LOGW(TAG, "job queue full");
}

void hermes_turn_cancel(void)
{
    link.recording = false;
    link.cancelled = true;
    link_job_t job = {.kind = JOB_CANCEL};
    if (xQueueSend(link.jobs, &job, 0) != pdTRUE) ESP_LOGW(TAG, "cancel dropped");
}

hermes_ev_t hermes_turn_event(char *text, size_t cap)
{
    link_event_t e;
    if (xQueueReceive(link.events, &e, 0) != pdTRUE) return HERMES_EV_NONE;
    if (e.kind == HERMES_EV_REPLY) reply_pending = false;
    if (link.cancelled && e.kind != HERMES_EV_ERROR) {
        /* Events from an abandoned turn are stale by definition. */
        return HERMES_EV_NONE;
    }
    hermes_copy_text(text, cap, e.text);
    return e.kind;
}

bool hermes_text_begin(const char *text)
{
    if (!text || !text[0]) return false;
    char *copy = strdup(text);
    if (!copy) return false;
    link_job_t job = {.kind = JOB_TEXT, .text = copy};
    if (xQueueSend(link.jobs, &job, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(copy);
        return false;
    }
    return true;
}

bool hermes_reply_copy(char *out, size_t cap)
{
    if (!cap) return false;
    xSemaphoreTake(link.lock, portMAX_DELAY);
    size_t n = link.reply_len;
    if (n >= cap) n = cap - 1;
    memcpy(out, link.reply, n);
    out[n] = '\0';
    bool any = link.reply_len > 0;
    xSemaphoreGive(link.lock);
    return any;
}

void hermes_link_config_changed(void)
{
    hermes_ws_stop();
    xSemaphoreTake(link.lock, portMAX_DELAY);
    set_state(&link, HERMES_CONNECTING, "配置已变更");
    xSemaphoreGive(link.lock);
}
