// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "esp_err.h"

enum { COMPANION_SERVER_URL_CAP=256, COMPANION_TOKEN_CAP=128 };

/* Hermes is the only backend: the gadget WebSocket URL of the gateway plugin
 * (ws://host:8765/gadget or wss://...) plus the optional shared access token
 * the host demands in hello when GADGET_ACCESS_TOKEN is set. Empty token
 * means the host runs without one. */
typedef struct {
    const char *server_url, *access_token;
} companion_credentials_config_t;

/* App-lifetime immutable snapshot after init. Only the setup portal saves a
 * replacement; it restarts the device before any service uses it. */
esp_err_t companion_credentials_init(void);
bool companion_credentials_ready(void);
bool companion_credentials_valid(const companion_credentials_config_t *config);
esp_err_t companion_credentials_save(const companion_credentials_config_t *config);
const char *companion_credentials_server_url(void);
const char *companion_credentials_access_token(void);
