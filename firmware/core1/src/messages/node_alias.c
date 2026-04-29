/* node_alias.c — see node_alias.h.
 *
 * Linear-scan table in PSRAM. 16 entries × ~40 B = 640 B PSRAM.
 * No flash persistence in v1 — aliases reset on reboot.
 *
 * Eviction: when the table is full and a fresh node comes in, drop
 * the entry with the smallest `last_touch_ms`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "node_alias.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

typedef struct {
    bool     in_use;
    uint32_t node_num;
    uint32_t last_touch_ms;
    char     text[NODE_ALIAS_MAX_LEN + 1];   /* NUL-terminated */
    uint16_t text_len;
} alias_entry_t;

static alias_entry_t s_table[NODE_ALIAS_CAP] __attribute__((section(".psram_bss")));
static bool          s_inited;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void node_alias_init(void)
{
    if (s_inited) return;
    /* PSRAM .bss is zero-initialised by C startup, so the in_use=false
     * check holds without an explicit memset here. */
    s_inited = true;
}

static int find_slot(uint32_t node_num)
{
    for (int i = 0; i < (int)NODE_ALIAS_CAP; ++i) {
        if (s_table[i].in_use && s_table[i].node_num == node_num) return i;
    }
    return -1;
}

static int find_free_or_evict(void)
{
    for (int i = 0; i < (int)NODE_ALIAS_CAP; ++i) {
        if (!s_table[i].in_use) return i;
    }
    /* Evict oldest. */
    int     victim = 0;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < (int)NODE_ALIAS_CAP; ++i) {
        if (s_table[i].last_touch_ms < oldest) {
            oldest = s_table[i].last_touch_ms;
            victim = i;
        }
    }
    return victim;
}

bool node_alias_set(uint32_t node_num, const char *text, uint16_t text_len)
{
    if (node_num == 0u) return false;
    node_alias_init();

    int idx = find_slot(node_num);

    /* Clear request */
    if (text == NULL || text_len == 0u) {
        if (idx >= 0) {
            memset(&s_table[idx], 0, sizeof(s_table[idx]));
        }
        return true;
    }

    if (idx < 0) idx = find_free_or_evict();

    alias_entry_t *e = &s_table[idx];
    if (text_len > NODE_ALIAS_MAX_LEN) text_len = NODE_ALIAS_MAX_LEN;
    /* UTF-8 truncation safety: if mid-multibyte, drop trailing bytes
     * until we land on a valid boundary so the displayed name doesn't
     * render as a replacement glyph. */
    while (text_len > 0u &&
           (((uint8_t)text[text_len] & 0xC0u) == 0x80u)) {
        text_len--;
    }
    memcpy(e->text, text, text_len);
    e->text[text_len] = '\0';
    e->text_len       = text_len;
    e->node_num       = node_num;
    e->in_use         = true;
    e->last_touch_ms  = now_ms();
    return true;
}

const char *node_alias_lookup(uint32_t node_num)
{
    if (node_num == 0u) return NULL;
    int idx = find_slot(node_num);
    return (idx >= 0) ? s_table[idx].text : NULL;
}

size_t node_alias_format_display(uint32_t node_num,
                                 const char *short_name,
                                 char *out, size_t cap)
{
    if (!out || cap == 0u) return 0u;
    const char *alias = node_alias_lookup(node_num);
    int n;
    if (alias && alias[0] != '\0') {
        n = snprintf(out, cap, "%s", alias);
    } else if (short_name && short_name[0] != '\0') {
        n = snprintf(out, cap, "%s", short_name);
    } else {
        n = snprintf(out, cap, "!%08lx", (unsigned long)node_num);
    }
    if (n < 0) return 0u;
    return ((size_t)n < cap) ? (size_t)n : (cap - 1u);
}
