// SPDX-License-Identifier: Apache-2.0
// One long-lived Hermes gadget WebSocket:
//   ws(s)://host:port/path   subprotocol "hermes-gadget.v1"
//   text frames  : JSON control frames, reassembled in a PSRAM buffer that
//                  grows to the frame size (reply.delta carries the whole
//                  cumulative answer, so frames can reach tens of KB)
//   binary frames: media downlink — this device declares no speaker and no
//                  image box, so the server never sends any; ignored here
// Everything the device sends goes through hermes_ws_send_* from the session
// task; the event callback only classifies and stores.
#include "hermes_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hermes_ws";
enum {
    WS_BUFFER = 1024,          /* rx chunk; larger frames arrive in pieces */
    WS_FRAME_MIN = 4096,       /* initial reassembly buffer */
    WS_FRAME_MAX = 72 * 1024,  /* REPLY_CAP (64 KiB) + JSON slack; larger frames
                                * are dropped whole rather than killing the link */
    WS_TASK_STACK = 8192,
    WS_URI_CAP = HERMES_HOST_CAP + HERMES_PATH_CAP + 32,
    WS_HANDSHAKE_WAIT_MS = 15000,
};

static struct {
    esp_websocket_client_handle_t client;
    hermes_ws_handlers_t handlers;
    char *frame;               /* PSRAM reassembly buffer */
    size_t frame_cap;
    bool dropping, open;
    int64_t last_rx_us;
} ws;

/* esp_websocket_register_events stores a plain esp_event_handler_t, so the
 * callback's argument types come from the event loop, not the websocket header. */
static void on_event(void *ctx, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)ctx; (void)base;
    esp_websocket_event_data_t *data = event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ws.open = true;
        ws.dropping = false;
        ESP_LOGI(TAG, "socket up");
        if (ws.handlers.on_open) ws.handlers.on_open(ws.handlers.ctx);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED: {
        ws.open = false;
        int status = data ? data->error_handle.esp_ws_handshake_status_code : 0;
        ESP_LOGW(TAG, "socket down (handshake %d)", status);
        if (ws.handlers.on_lost) ws.handlers.on_lost(ws.handlers.ctx, status);
        break;
    }
    case WEBSOCKET_EVENT_DATA: {
        ws.last_rx_us = esp_timer_get_time();
        if (data->op_code == WS_TRANSPORT_OPCODES_BINARY || data->data_len < 0) break;
        if (data->payload_offset == 0) {
            size_t total = (size_t)data->payload_len;
            ws.dropping = total + 1 > WS_FRAME_MAX;
            if (!ws.dropping && total + 1 > ws.frame_cap) {
                /* Reply deltas grow with the answer, so the buffer follows the
                 * frames; a shrink back happens on the next small frame only
                 * via a fresh allocation, which is not worth the churn. */
                size_t cap = ws.frame_cap ? ws.frame_cap : WS_FRAME_MIN;
                while (cap < total + 1) cap *= 2;
                char *grown = heap_caps_realloc(ws.frame, cap,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (grown) { ws.frame = grown; ws.frame_cap = cap; }
                else ws.dropping = true;
            }
        }
        if (ws.dropping) break;
        size_t end = (size_t)data->payload_offset + (size_t)data->data_len;
        if (end + 1 > ws.frame_cap) { ws.dropping = true; break; }
        memcpy(ws.frame + data->payload_offset, data->data_ptr, (size_t)data->data_len);
        ws.frame[end] = '\0';
        if (data->fin && end == (size_t)data->payload_len) {
            if (ws.handlers.on_text) ws.handlers.on_text(ws.handlers.ctx, ws.frame);
            ws.frame[0] = '\0';
        }
        break;
    }
    default:
        break;
    }
}

bool hermes_ws_start(const hermes_url_t *url, const hermes_ws_handlers_t *handlers)
{
    if (!url || !url->host[0] || !handlers) return false;
    hermes_ws_stop();
    ws.handlers = *handlers;
    static char uri[WS_URI_CAP];
    unsigned port = url->port ? url->port : (url->tls ? 443 : 8765);
    int n = snprintf(uri, sizeof(uri), "%s://%s:%u%s",
                     url->tls ? "wss" : "ws", url->host, port, url->path);
    if (n < 0 || (size_t)n >= sizeof(uri)) return false;
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .subprotocol = "hermes-gadget.v1",
        .disable_auto_reconnect = true,
        .buffer_size = WS_BUFFER,
        .task_stack = WS_TASK_STACK,
        .network_timeout_ms = 15000,
    };
    ws.client = esp_websocket_client_init(&cfg);
    if (!ws.client) return false;
    esp_websocket_register_events(ws.client, WEBSOCKET_EVENT_ANY, on_event, NULL);
    if (esp_websocket_client_start(ws.client) != ESP_OK) {
        esp_websocket_client_destroy(ws.client);
        ws.client = NULL;
        return false;
    }
    for (unsigned waited = 0; waited < WS_HANDSHAKE_WAIT_MS && !ws.open; waited += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (!ws.open) {
        ESP_LOGW(TAG, "transport handshake did not complete");
        hermes_ws_stop();
        return false;
    }
    return true;
}

bool hermes_ws_open(void) { return ws.client && ws.open; }

bool hermes_ws_send_text(const char *frame, size_t len)
{
    if (!hermes_ws_open() || !frame) return false;
    int sent = esp_websocket_client_send_text(ws.client, frame, (int)len,
                                              pdMS_TO_TICKS(5000));
    /* send_text returns the byte count it wrote, not ESP_OK. */
    if (sent < 0 || (size_t)sent != len) {
        ESP_LOGW(TAG, "text send failed (sent=%d want=%u)", sent, (unsigned)len);
        ws.open = false;   /* a rejected write means the transport is gone */
        return false;
    }
    return true;
}

bool hermes_ws_send_binary(uint8_t channel, uint8_t stream, uint16_t seq,
                           const uint8_t *payload, size_t len)
{
    if (!hermes_ws_open() || !payload || !len) return false;
    uint8_t frame[4 + 1024];
    size_t chunk = len > sizeof(frame) - 4 ? sizeof(frame) - 4 : len;
    frame[0] = channel;
    frame[1] = stream;
    frame[2] = (uint8_t)(seq & 0xff);
    frame[3] = (uint8_t)(seq >> 8);
    memcpy(frame + 4, payload, chunk);
    int sent = esp_websocket_client_send_bin(ws.client, (const char *)frame,
                                             (int)(chunk + 4), pdMS_TO_TICKS(5000));
    if (sent < 0 || (size_t)sent != chunk + 4) {
        ESP_LOGW(TAG, "binary send failed (sent=%d want=%u)", sent, (unsigned)(chunk + 4));
        ws.open = false;
        return false;
    }
    return true;
}

void hermes_ws_stop(void)
{
    if (!ws.client) return;
    esp_websocket_client_stop(ws.client);
    esp_websocket_client_destroy(ws.client);
    ws.client = NULL;
    ws.open = false;
    ws.dropping = false;
    if (ws.frame) ws.frame[0] = '\0';
}

int64_t hermes_ws_last_rx_us(void) { return ws.last_rx_us; }
