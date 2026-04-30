/* settings_tree.c — see settings_tree.h.
 *
 * Storage model (one big static array):
 *   slot 0:        root
 *   slot 1..15:    group nodes (one per settings_group_t)
 *   slot 16..N:    leaf nodes (one per IPC_CFG_* key)
 *
 * Each node carries its kind, parent slot, and (for leaves) a back-
 * pointer to the settings_key_def_t inside settings_keys.c. Children
 * are not stored explicitly — they're enumerated by walking the table
 * for entries whose parent matches. Cheap because the total node
 * count is < 110.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "settings_tree.h"

#include <stdio.h>
#include <string.h>

/* Internal layout — exposed to settings_tree.h only as opaque
 * "settings_tree_node_t *". */
struct settings_tree_node {
    uint8_t                          kind;        /* settings_tree_node_kind_t */
    uint8_t                          parent_slot; /* index into s_nodes[] */
    uint16_t                         _rsv;
    /* For groups: the matching settings_group_t value (group index).
     * For leaves: pointer back into settings_keys.c's static table.
     * For root: both unused. */
    settings_group_t                 group_id;
    const settings_key_def_t        *key;
    const char                      *label_override; /* root = "Settings" */
};

/* Bound check: 1 (root) + 15 (groups) + 88 (current keys) = 104.
 * Capped exactly at 104 (zero headroom) — Core 1 BSS is tight after
 * the C-2/C-3/C-4 view structs landed and we already moved their
 * state into PSRAM. Each node = 16 B → 1.62 KB BSS. Adding a key
 * to settings_keys.c WILL trip build_once()'s ST_NODE_MAX guard
 * silently (extra keys past the cap are dropped); raise this cap
 * AND free more RAM elsewhere when that happens. */
#define ST_NODE_MAX  104

static struct settings_tree_node s_nodes[ST_NODE_MAX];
static uint8_t                   s_node_count;
static bool                      s_built;

static void build_once(void)
{
    if (s_built) return;

    /* Slot 0 = root */
    s_nodes[0].kind        = ST_NODE_ROOT;
    s_nodes[0].parent_slot = 0;            /* self-reference; parent() returns NULL on root */
    s_nodes[0].label_override = "Settings";

    s_node_count = 1;

    /* Slots 1..SG_GROUP_COUNT = group nodes (1 per group, in enum order) */
    for (uint8_t g = 0; g < SG_GROUP_COUNT && s_node_count < ST_NODE_MAX; ++g) {
        struct settings_tree_node *n = &s_nodes[s_node_count];
        n->kind        = ST_NODE_GROUP;
        n->parent_slot = 0;                /* parent = root */
        n->group_id    = (settings_group_t)g;
        n->key         = NULL;
        n->label_override = NULL;
        s_node_count++;
    }

    /* Then leaf nodes. Walk each group, collect every settings_key_def_t. */
    for (uint8_t g = 0; g < SG_GROUP_COUNT; ++g) {
        uint8_t n_keys = 0;
        const settings_key_def_t *keys =
            settings_keys_in_group((settings_group_t)g, &n_keys);
        if (!keys || n_keys == 0) continue;

        /* parent slot for this group's leaves = 1 + g (we filled groups
         * starting at slot 1 in enum order). */
        uint8_t parent = (uint8_t)(1u + g);

        for (uint8_t i = 0; i < n_keys && s_node_count < ST_NODE_MAX; ++i) {
            struct settings_tree_node *n = &s_nodes[s_node_count];
            n->kind        = ST_NODE_LEAF;
            n->parent_slot = parent;
            n->group_id    = (settings_group_t)g;
            n->key         = &keys[i];
            n->label_override = NULL;
            s_node_count++;
        }
    }

    s_built = true;
}

static settings_tree_node_t *node_at(uint8_t slot)
{
    return &s_nodes[slot];
}

static uint8_t slot_of(settings_tree_node_t *n)
{
    /* Pointer arithmetic into the static array. n must come from this
     * module so this is always defined. */
    const struct settings_tree_node *base = s_nodes;
    return (uint8_t)(n - base);
}

settings_tree_node_t *settings_tree_root(void)
{
    build_once();
    return node_at(0);
}

settings_tree_node_t *settings_tree_group_node(settings_group_t g)
{
    if ((unsigned)g >= SG_GROUP_COUNT) return NULL;
    build_once();
    /* Group nodes occupy slots 1..SG_GROUP_COUNT in enum order. */
    return node_at((uint8_t)(1u + (unsigned)g));
}

settings_tree_node_kind_t settings_tree_node_kind(settings_tree_node_t *n)
{
    if (!n) return ST_NODE_ROOT;
    return (settings_tree_node_kind_t)n->kind;
}

const char *settings_tree_node_label(settings_tree_node_t *n)
{
    if (!n) return "";
    if (n->label_override) return n->label_override;
    if (n->kind == ST_NODE_GROUP) {
        return settings_group_name(n->group_id);
    }
    if (n->kind == ST_NODE_LEAF && n->key) {
        return n->key->label ? n->key->label : "(unnamed)";
    }
    return "";
}

const settings_key_def_t *settings_tree_node_key(settings_tree_node_t *n)
{
    if (!n || n->kind != ST_NODE_LEAF) return NULL;
    return n->key;
}

uint16_t settings_tree_child_count(settings_tree_node_t *n)
{
    if (!n) return 0;
    build_once();
    if (n->kind == ST_NODE_LEAF) return 0;

    uint8_t target_slot = slot_of(n);
    uint16_t count = 0;
    for (uint8_t i = 1; i < s_node_count; ++i) {
        if (s_nodes[i].parent_slot == target_slot) count++;
    }
    return count;
}

settings_tree_node_t *settings_tree_child_at(settings_tree_node_t *n,
                                              uint16_t idx)
{
    if (!n) return NULL;
    build_once();
    if (n->kind == ST_NODE_LEAF) return NULL;

    uint8_t target_slot = slot_of(n);
    uint16_t seen = 0;
    for (uint8_t i = 1; i < s_node_count; ++i) {
        if (s_nodes[i].parent_slot != target_slot) continue;
        if (seen == idx) return node_at(i);
        seen++;
    }
    return NULL;
}

settings_tree_node_t *settings_tree_parent(settings_tree_node_t *n)
{
    if (!n) return NULL;
    if (n->kind == ST_NODE_ROOT) return NULL;
    return node_at(n->parent_slot);
}

size_t settings_tree_format_breadcrumb(settings_tree_node_t *n,
                                        char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!n) return 0;

    /* Walk up collecting labels (leaf-first), reverse on emit. */
    settings_tree_node_t *chain[8];   /* max depth = root + group + leaf = 3, headroom for future */
    int depth = 0;
    settings_tree_node_t *cur = n;
    while (cur && depth < 8) {
        chain[depth++] = cur;
        cur = settings_tree_parent(cur);
    }

    /* Emit root → leaf with " > " separator. Use plain ASCII '>' for
     * stability across font sets; the spec uses "›" but that needs the
     * Unifont CJK glyph subset which we have. Keep ASCII for now. */
    size_t off = 0;
    for (int i = depth - 1; i >= 0 && off < cap - 1; --i) {
        const char *lbl = settings_tree_node_label(chain[i]);
        if (i < depth - 1) {
            int n_w = snprintf(out + off, cap - off, " > ");
            if (n_w < 0 || (size_t)n_w >= cap - off) break;
            off += (size_t)n_w;
        }
        int n_w = snprintf(out + off, cap - off, "%s", lbl);
        if (n_w < 0) break;
        if ((size_t)n_w >= cap - off) {
            off = cap - 1;
            break;
        }
        off += (size_t)n_w;
    }
    out[off] = '\0';
    return off;
}
