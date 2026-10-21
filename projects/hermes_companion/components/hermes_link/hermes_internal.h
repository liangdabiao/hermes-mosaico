// SPDX-License-Identifier: Apache-2.0
// Internal shared surface of the Hermes link: the WebSocket wrapper both
// sides of the component use.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hermes_wire.h"

/* ---- WebSocket (singleton: one long-lived connection) ---- */
typedef struct {
    /* A complete JSON text frame (NUL-terminated). Runs on the client task. */
    void (*on_text)(void *ctx, const char *frame);
    void (*on_open)(void *ctx);
    /* handshake_status is the HTTP code of a failed upgrade, 0 for a drop. */
    void (*on_lost)(void *ctx, int handshake_status);
    void *ctx;
} hermes_ws_handlers_t;

bool hermes_ws_start(const hermes_url_t *url, const hermes_ws_handlers_t *handlers);
bool hermes_ws_open(void);
/* Sends from any task except the websocket client's own: the session task
 * owns every frame the device emits. */
bool hermes_ws_send_text(const char *frame, size_t len);
bool hermes_ws_send_binary(uint8_t channel, uint8_t stream, uint16_t seq,
                           const uint8_t *payload, size_t len);
void hermes_ws_stop(void);
/* Monotonic microseconds of the last received frame (dead-link detection). */
int64_t hermes_ws_last_rx_us(void);
