/* node_alias.h — Per-peer local nicknames.
 *
 * Lets the user rename a peer node locally without touching the
 * mesh-broadcast NodeInfo. Aliases are display-only, kept on Core 1
 * in PSRAM (no flash persistence in v1 — they survive across LVGL
 * task restarts but not across MCU reboot).
 *
 * Lookup priority for a "show name for node X" call:
 *   1. node_alias_lookup(X)        — if user set a local alias
 *   2. phoneapi_node_t.short_name  — broadcast short name
 *   3. "!XXXXXXXX"                 — hex of node id
 *
 * Capacity: 16 entries × 32 B + bookkeeping. Eviction picks the
 * least-recently-touched slot (write timestamp).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_ALIAS_MAX_LEN  31u    /* +NUL = 32 B per entry buffer  */
#define NODE_ALIAS_CAP      16u    /* total entries                 */

/** Lazy-init. Idempotent; safe pre-scheduler. */
void node_alias_init(void);

/** Set or clear an alias. Empty `text` (or NULL / len=0) clears the
 *  entry for this node id. Returns true on success / on clear. */
bool node_alias_set(uint32_t node_num, const char *text, uint16_t text_len);

/** Lookup. Returns NULL if no alias is set. The returned pointer is
 *  valid until the next node_alias_set call for any node. */
const char *node_alias_lookup(uint32_t node_num);

/** Convenience: copy `out`-sized display name (alias if set, else
 *  short_name fallback, else "!XXXXXXXX" hex). short_name may be
 *  NULL or empty. Returns chars written (excluding NUL). */
size_t node_alias_format_display(uint32_t node_num,
                                 const char *short_name,
                                 char *out, size_t cap);

#ifdef __cplusplus
}
#endif
