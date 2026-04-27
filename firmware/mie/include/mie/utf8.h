/* mie/utf8.h — small UTF-8 helpers shared across MIE callers.
 *
 * Placed in <mie/...> rather than near a single caller because every
 * site that lets a user produce a UTF-8 string needs the same
 * boundary-aware truncation discipline (settings strings, message
 * outbound, channel name, future search box, etc.). Keeping one
 * implementation under host unit tests prevents the cut-mid-codepoint
 * class of bug from creeping back in via copy-paste.
 *
 * The header is C-compatible — works from C, C++, both cores, and the
 * host test target with no dependencies beyond <stddef.h>.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MIE_UTF8_H
#define MIE_UTF8_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * mie_utf8_truncate — return the largest byte length ≤ min(len, max_bytes)
 * that ends on a UTF-8 codepoint boundary.
 *
 * The input is assumed to be well-formed UTF-8 (validation is the
 * caller's responsibility — IME-produced strings are well-formed by
 * construction). This walks back from `max_bytes` until landing on a
 * byte whose top two bits are NOT `10` (continuation marker), so a
 * multi-byte sequence is never cut in the middle.
 *
 * Edge cases:
 *   - `len <= max_bytes`            → returns `len` (no work).
 *   - `max_bytes == 0`              → returns 0.
 *   - `s == NULL && len > 0`        → returns 0 (defensive).
 *   - lone continuation bytes near `max_bytes` → walks back past them
 *     until a start byte; result may be 0 if the leading bytes are
 *     all continuations (malformed input).
 *
 * Examples (with U+6211 = "我" encoded as E6 88 91):
 *   mie_utf8_truncate("我的", 6, 6) == 6   // exact fit
 *   mie_utf8_truncate("我的", 6, 4) == 3   // can't take 4 → drop second char
 *   mie_utf8_truncate("我的", 6, 3) == 3   // exact codepoint boundary
 *   mie_utf8_truncate("我的", 6, 2) == 0   // no full codepoint fits
 *   mie_utf8_truncate("ABCD", 4, 2) == 2   // ASCII trivial
 */
size_t mie_utf8_truncate(const char *s, size_t len, size_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* MIE_UTF8_H */
