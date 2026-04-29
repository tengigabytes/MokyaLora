/* template_text.h — leaf template D (Phase 4 spec §50).
 *
 * Used by settings_app_view for SK_KIND_STR keys (Long Name / Short
 * Name / PSK / etc — ~5 keys per spec §50 模板 D).
 *
 * Template D is a thin wrapper around the existing ime_request_text
 * Mode B (fullscreen) — there is no per-template widget tree to
 * render here. open() fires the IME request; the IME modal owns the
 * screen until the user commits or cancels. On callback, the host
 * polls done() / committed() / value() and packs the UTF-8 bytes
 * into an IPC SET.
 *
 * Lifecycle differs from the other templates:
 *   open()  - fire ime_request_text (IME opens as fullscreen modal,
 *             setting_app_view's panel goes invisible underneath)
 *   close() - no-op (the IME modal cleans itself up); included for
 *             API symmetry with the other templates
 *   apply_key() - returns false unconditionally; the IME captures
 *             every key while it owns the modal
 *   done()  - true once the IME callback has fired
 *   value() - pointer to the committed UTF-8 buffer (NUL-terminated,
 *             valid until the next open())
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "key_event.h"
#include "settings_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

void        template_text_open(const settings_key_def_t *key);
void        template_text_close(void);
bool        template_text_apply_key(const key_event_t *ev);
bool        template_text_done(void);
bool        template_text_committed(void);
const char *template_text_value(void);
uint16_t    template_text_value_len(void);

#ifdef __cplusplus
}
#endif
