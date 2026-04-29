/* settings_tree.h — Hierarchical model for the settings App (Phase 4).
 *
 * Replaces the flat 88-key settings_view with a 2-level tree:
 *   root → group (S-1..S-12 per docs/ui/01-page-architecture.md)
 *        → leaf (one of the existing IPC_CFG_* keys from settings_keys.h)
 *
 * The tree is statically built from settings_keys.h's group/key table —
 * no second source of truth. Group nodes are derived from the
 * settings_group_t enum; leaf nodes are derived per-key.
 *
 * Navigation API is intentionally tiny: a cursor that walks parent /
 * child / sibling so settings_app_view can render breadcrumb + child
 * list without knowing internal storage.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "settings_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ST_NODE_ROOT  = 0,    /* the implicit root above all S-* groups */
    ST_NODE_GROUP = 1,    /* S-1..S-12 group container               */
    ST_NODE_LEAF  = 2,    /* a single IPC_CFG_* key                  */
} settings_tree_node_kind_t;

/* Lightweight handle. Internally an index into a static table
 * maintained by settings_tree.c — never freed. NULL = "no node". */
typedef const struct settings_tree_node settings_tree_node_t;

settings_tree_node_t       *settings_tree_root(void);

settings_tree_node_kind_t   settings_tree_node_kind(settings_tree_node_t *n);

/* Display label for breadcrumb / list rendering. NULL-terminated.
 * Root node returns "Settings"; group nodes return the group name;
 * leaf nodes return the key's `label` from settings_keys. */
const char                 *settings_tree_node_label(settings_tree_node_t *n);

/* For leaf nodes, returns the underlying settings_key_def_t* so the
 * leaf-template view can fetch kind / range / enum. NULL for non-leaf. */
const settings_key_def_t   *settings_tree_node_key(settings_tree_node_t *n);

/* Number of children, and child by index. For leaves: returns 0 / NULL. */
uint16_t                    settings_tree_child_count(settings_tree_node_t *n);
settings_tree_node_t       *settings_tree_child_at(settings_tree_node_t *n,
                                                   uint16_t idx);

/* Parent walk. Root returns NULL. */
settings_tree_node_t       *settings_tree_parent(settings_tree_node_t *n);

/* Convenience: build a "›"-joined breadcrumb string from `n` upward.
 * Output is truncated to `cap-1` chars + NUL. Returns total chars
 * written (excluding NUL). Root → "Settings"; group → "Settings ›
 * <group>"; leaf → "Settings › <group> › <leaf>". */
size_t                      settings_tree_format_breadcrumb(
                                settings_tree_node_t *n,
                                char *out, size_t cap);

#ifdef __cplusplus
}
#endif
