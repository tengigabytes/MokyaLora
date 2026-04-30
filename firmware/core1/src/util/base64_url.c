/* base64_url.c — see base64_url.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "base64_url.h"

static const char s_alphabet[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t base64_urlsafe_encode(const uint8_t *in, size_t in_len,
                              char *out, size_t cap)
{
    if (out == NULL || cap == 0u) return 0u;
    size_t need = ((in_len + 2u) / 3u) * 4u + 1u;
    if (cap < need) return 0u;

    size_t o = 0u;
    size_t i = 0u;
    while (i + 3u <= in_len) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1u] << 8) |
                      (uint32_t)in[i + 2u];
        out[o + 0] = s_alphabet[(v >> 18) & 0x3Fu];
        out[o + 1] = s_alphabet[(v >> 12) & 0x3Fu];
        out[o + 2] = s_alphabet[(v >>  6) & 0x3Fu];
        out[o + 3] = s_alphabet[(v >>  0) & 0x3Fu];
        i += 3u;
        o += 4u;
    }
    /* Tail: 1 or 2 leftover bytes => 2 or 3 b64 chars, no padding. */
    if (i + 1u == in_len) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o + 0] = s_alphabet[(v >> 18) & 0x3Fu];
        out[o + 1] = s_alphabet[(v >> 12) & 0x3Fu];
        o += 2u;
    } else if (i + 2u == in_len) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1u] << 8);
        out[o + 0] = s_alphabet[(v >> 18) & 0x3Fu];
        out[o + 1] = s_alphabet[(v >> 12) & 0x3Fu];
        out[o + 2] = s_alphabet[(v >>  6) & 0x3Fu];
        o += 3u;
    }
    out[o] = '\0';
    return o;
}
