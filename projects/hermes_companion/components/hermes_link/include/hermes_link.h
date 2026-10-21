// SPDX-License-Identifier: Apache-2.0
// Hermes gadget link: Wi-Fi STA, device-key handshake, pairing, streaming
// push-to-talk and chat over one WebSocket to the Hermes gateway plugin.
// The app talks to this component only through these symbols.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* ---- Connection ---- */
typedef enum {
    HERMES_NOT_SET,      /* no server URL configured */
    HERMES_OFFLINE,      /* configured, Wi-Fi down */
    HERMES_CONNECTING,   /* WebSocket up, hello/challenge/auth/welcome running */
    HERMES_PAIRING,      /* enrolled, waiting for the host to approve the code */
    HERMES_ONLINE,       /* welcomed and paired: turns may run */
    HERMES_UNREACHABLE,  /* connect or handshake failed; retrying with backoff */
} hermes_state_t;
typedef struct { hermes_state_t state; char detail[48]; } hermes_status_t;

/* Starts the Wi-Fi and session tasks. Safe to call once per boot. */
esp_err_t hermes_link_start(void);
bool hermes_wifi_connected(void);
/* Wi-Fi up, credentials present, welcomed and paired. */
bool hermes_link_ready(void);
void hermes_link_status(hermes_status_t *out);
const char *hermes_state_name(hermes_state_t state);
/* The pairing code while state is HERMES_PAIRING, "" otherwise. */
const char *hermes_pairing_code(void);
/* Call after the portal writes new credentials (before reboot). */
void hermes_link_config_changed(void);

/* ---- Push-to-talk turns (voice worker only) ---- */
/* Credentials present and Wi-Fi up: a press may start a turn. */
bool hermes_turn_ready(void);
/* Press: start capturing speech into the PSRAM clip. */
void hermes_turn_begin(void);
/* 16 kHz mono frames, in order, from press to release. Buffered in the clip
 * and streamed to the server in binary frames by the session task. */
void hermes_turn_audio(const int16_t *pcm, size_t frames);
/* Release: flush what has not been sent, close the utterance, wait for the
 * transcript / streamed reply. */
void hermes_turn_end(void);
/* Abandon: drop the clip, and cancel a running turn on the server. */
void hermes_turn_cancel(void);

typedef enum {
    HERMES_EV_NONE,
    HERMES_EV_HEARD,   /* transcript of the spoken question (host STT) */
    HERMES_EV_REPLY,   /* reply text so far (cumulative snapshot) */
    HERMES_EV_DONE,    /* reply complete; read it with hermes_reply_copy */
    HERMES_EV_ERROR,   /* turn failed; the event text says why */
} hermes_ev_t;
/* Non-blocking; copies the event text. Events of cancelled turns are dropped. */
hermes_ev_t hermes_turn_event(char *text, size_t cap);

/* ---- Typed turns (share the same transport, used by the Iris service) ---- */
typedef void (*hermes_text_cb)(void *ctx, const char *event, const char *text);
/* event is "start" | "delta" | "done" | "error"; register before tasks start. */
void hermes_text_observer(hermes_text_cb cb, void *ctx);
bool hermes_text_begin(const char *text);
/* Copy the current reply, up to cap-1 bytes; true when there is any. */
bool hermes_reply_copy(char *out, size_t cap);
/* Kept for API symmetry with the retired Octop link; the Hermes protocol
 * carries no language hint, so this is a no-op. */
void hermes_language(unsigned language);
