// SPDX-License-Identifier: Apache-2.0
/* Self-contained SHA-256 / HMAC-SHA256 / Base64 for the Hermes gadget
 * handshake. Kept in-tree (instead of calling a platform crypto library) so
 * the device firmware and the host unit tests run the exact same code path —
 * the same discipline the reference SDK applies on both sides of the wire. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* out = 32 raw bytes of SHA-256 over the message. */
void hermes_sha256(const uint8_t *msg, size_t len, uint8_t out[32]);

/* out = 32 raw bytes of HMAC-SHA256(key, msg). Any key length >= 1. */
void hermes_hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/* Lowercase hex of `len` bytes into out (needs len*2+1 bytes). */
void hermes_hex(const uint8_t *in, size_t len, char *out);

/* Base64 (standard alphabet, padded). Returns the written length excluding
 * the terminator, or 0 when out_cap is too small (needed = 4*ceil(len/3)+1). */
size_t hermes_base64_encode(const uint8_t *in, size_t len, char *out, size_t out_cap);

/* Decodes standard or URL-safe base64, with or without padding. Returns the
 * decoded byte count, or (size_t)-1 on malformed input / no room. */
size_t hermes_base64_decode(const char *in, uint8_t *out, size_t out_cap);

/* "hg-" + first 16 hex chars of SHA-256(key) into out (needs 20 bytes). */
void hermes_device_id(const uint8_t key[32], char out[20]);
