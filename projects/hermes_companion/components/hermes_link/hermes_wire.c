// SPDX-License-Identifier: Apache-2.0
// Pure, host-testable helpers for the Hermes link: ws:// URL splitting, JSON
// member extraction without a parser, and JSON string escaping. The device
// firmware and the host tests compile this same file.
#include "hermes_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool hermes_copy_text(char *dst, size_t cap, const char *src)
{
    if (!cap) return false;
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
    return src[n] == '\0';
}

/* "ws://host[:port][/path]" / "wss://..." into parts. Defaults: port 80/441
 * would be wrong for this protocol — the hub's default is 8765, and a bare
 * host with no port means exactly that, so an omitted port stays 0 and the
 * caller substitutes 8765 (ws) / 443 is NOT assumed for wss either; the
 * provisioning page always shows the full URL). */
bool hermes_url_split(const char *url, hermes_url_t *out)
{
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));
    bool tls = !strncmp(url, "wss://", 6);
    size_t prefix = tls ? 6 : (!strncmp(url, "ws://", 5) ? 5 : 0);
    if (!prefix || !url[prefix]) return false;
    const char *host = url + prefix;
    const char *slash = strchr(host, '/');
    size_t host_len = slash ? (size_t)(slash - host) : strlen(host);
    const char *colon = memchr(host, ':', host_len);
    if (colon) {
        host_len = (size_t)(colon - host);
        long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535) return false;
        out->port = (uint16_t)port;
    }
    if (!host_len || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, host, host_len);
    out->host[host_len] = '\0';
    out->tls = tls;
    if (slash) {
        if (!hermes_copy_text(out->path, sizeof(out->path), slash)) return false;
    } else {
        hermes_copy_text(out->path, sizeof(out->path), "/gadget");
    }
    return true;
}

/* ---- JSON member extraction (no parser, no heap) ----
 * Finds `"key"` at object depth 1 only (a value containing the same quoted
 * key text would otherwise match inside a string), then reads the value. */

static const char *find_member(const char *json, const char *key)
{
    size_t key_len = strlen(key);
    const char *p = json;
    unsigned depth = 0;
    while (*p) {
        if (*p == '{' || *p == '[') ++depth;
        else if (*p == '}' || *p == ']') { if (depth) --depth; }
        else if (*p == '"' && depth <= 1) {
            const char *start = p + 1;
            const char *end = start;
            while (*end && *end != '"') {
                if (*end == '\\' && end[1]) ++end;
                ++end;
            }
            if (*end != '"') return NULL;
            if ((size_t)(end - start) == key_len && !memcmp(start, key, key_len)) {
                const char *colon = end + 1;
                while (*colon == ' ' || *colon == ':') ++colon;
                if (colon != end + 1) return colon;
            }
            p = end;
        }
        ++p;
    }
    return NULL;
}

static void put_utf8(char *out, size_t cap, size_t *o, unsigned cp)
{
    unsigned char b[4];
    unsigned n;
    if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xc0 | (cp >> 6); b[1] = 0x80 | (cp & 63); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xe0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 63); b[2] = 0x80 | (cp & 63); n = 3; }
    else { b[0] = 0xf0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 63); b[2] = 0x80 | ((cp >> 6) & 63); b[3] = 0x80 | (cp & 63); n = 4; }
    if (*o + n >= cap) return;
    memcpy(out + *o, b, n);
    *o += n;
}

bool hermes_json_string(const char *json, const char *key, char *out, size_t cap)
{
    if (!cap) return false;
    out[0] = '\0';
    const char *v = find_member(json, key);
    if (!v || *v != '"') return false;
    ++v;
    size_t o = 0;
    while (*v && *v != '"') {
        if (*v == '\\') {
            ++v;
            switch (*v) {
            case '"': case '\\': case '/':
                if (o + 1 < cap) { out[o++] = *v; }
                break;
            case 'n': if (o + 1 < cap) out[o++] = '\n'; break;
            case 'r': if (o + 1 < cap) out[o++] = '\r'; break;
            case 't': if (o + 1 < cap) out[o++] = '\t'; break;
            case 'b': case 'f': break;
            case 'u': {
                unsigned cp = 0;
                for (unsigned i = 1; i <= 4 && v[i]; ++i) {
                    char c = v[i];
                    unsigned d = (unsigned)(c >= '0' && c <= '9' ? c - '0'
                                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0x10);
                    if (d > 15) { cp = 0; break; }
                    cp = cp * 16 + d;
                }
                if (cp) put_utf8(out, cap, &o, cp);
                v += 4;
                break;
            }
            default: break;
            }
            if (!*v) break;
            ++v;
        } else if (o + 1 < cap) {
            out[o++] = *v;
            ++v;
        } else {
            ++v;
        }
    }
    out[o] = '\0';
    return *v == '"';
}

bool hermes_json_bool(const char *json, const char *key, bool *out)
{
    const char *v = find_member(json, key);
    if (!v) return false;
    if (!strncmp(v, "true", 4)) { *out = true; return true; }
    if (!strncmp(v, "false", 5)) { *out = false; return true; }
    return false;
}

bool hermes_json_long(const char *json, const char *key, long *out)
{
    const char *v = find_member(json, key);
    if (!v) return false;
    char *end;
    long value = strtol(v, &end, 10);
    if (end == v) return false;
    *out = value;
    return true;
}

/* Escape `text` into `out` as a JSON string body (no quotes). Returns false
 * when the escaped form does not fit. Control characters other than the
 * named escapes are dropped, matching the server's tolerance. */
bool hermes_json_escape(const char *text, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *s = text; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        size_t need = (c == '"' || c == '\\' || c < 0x20) ? 2 : 1;
        if (o + need >= cap) return false;
        switch (c) {
        case '"': out[o++] = '\\'; out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
        case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
        case '\t': out[o++] = '\\'; out[o++] = 't'; break;
        default:
            if (c < 0x20) break;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return true;
}
