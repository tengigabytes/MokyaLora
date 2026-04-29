/* canned_messages.c — see canned_messages.h.
 *
 * v1 ships a hardcoded English/Chinese list. UTF-8 strings are fine
 * for storage and for `messages_send_text` (the cascade encoder treats
 * the body as opaque bytes); rendering relies on the global UI font
 * (`ui_font_sm16` → MIEF unifont covering Latin-1 + Bopomofo + CJK).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "canned_messages.h"

#include <stddef.h>

static const char *const DEFAULTS[] = {
    "OK",
    "Yes",
    "No",
    "Thanks",
    "On my way",
    "Be there in 5",
    "I'm safe",
    "Stand by",
};

#define DEFAULT_COUNT  ((uint8_t)(sizeof(DEFAULTS) / sizeof(DEFAULTS[0])))

uint8_t canned_count(void)
{
    return DEFAULT_COUNT;
}

const char *canned_at(uint8_t idx)
{
    if (idx >= DEFAULT_COUNT) return NULL;
    return DEFAULTS[idx];
}
