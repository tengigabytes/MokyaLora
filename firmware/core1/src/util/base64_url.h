/* base64_url.h — RFC 4648 §5 URL-safe base64 (`-_`, no padding).
 *
 * Used by B-4 channel-share URL builder. Encoder only (decoder isn't
 * needed by Core 1 — host parses the URL, not us).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode `in_len` bytes from `in` into URL-safe base64 (no padding)
 * at `out`, NUL-terminated. Output length is ceil(in_len * 4 / 3),
 * unpadded (i.e. trailing `=` chars are dropped).
 *
 * Returns the number of characters written (excluding the NUL), or
 * 0 if `cap` is too small. Caller should size `cap >= 4 * ceil(in_len/3)
 * + 1` to be safe. */
size_t base64_urlsafe_encode(const uint8_t *in, size_t in_len,
                              char *out, size_t cap);

#ifdef __cplusplus
}
#endif
