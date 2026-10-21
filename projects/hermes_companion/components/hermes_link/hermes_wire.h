// SPDX-License-Identifier: Apache-2.0
// Shared pure helpers: ws:// URL split, JSON member reads, JSON escaping.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HERMES_HOST_CAP = 96,
    HERMES_PATH_CAP = 192,
    HERMES_DETAIL_CAP = 48,
    HERMES_CODE_CAP = 12,      /* pairing code + slack */
    HERMES_NONCE_CAP = 32,     /* base64 of 16 bytes */
    HERMES_MAC_CAP = 48,       /* base64 of 32 bytes */
};

typedef struct {
    char host[HERMES_HOST_CAP];
    uint16_t port;              /* 0 = use the protocol default (8765) */
    bool tls;
    char path[HERMES_PATH_CAP]; /* always starts with '/' */
} hermes_url_t;

/* "ws://host[:port][/path]" / "wss://..." into parts; false on anything else.
 * A path is kept as given (reverse-proxy subpath); default "/gadget". */
bool hermes_url_split(const char *url, hermes_url_t *out);

/* Bounded copy; true when the whole source fit. */
bool hermes_copy_text(char *dst, size_t cap, const char *src);

/* Read one string / bool / long member out of a flat JSON object. String
 * values unescape \" \\ \n \r \t and \uXXXX (BMP). */
bool hermes_json_string(const char *json, const char *key, char *out, size_t cap);
bool hermes_json_bool(const char *json, const char *key, bool *out);
bool hermes_json_long(const char *json, const char *key, long *out);

/* Escape into a JSON string body (no surrounding quotes). */
bool hermes_json_escape(const char *text, char *out, size_t cap);
