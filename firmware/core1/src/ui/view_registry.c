/* view_registry.c — central table of view_descriptor_t pointers,
 * indexed by view_id_t. Each view exports a `*_view_descriptor()`
 * getter; this file collates them.
 *
 * The table is mutable-pointer-to-const-descriptor so it can be
 * populated once during view_router_init() — see view_router.c
 * (avoids `__attribute__((constructor))` ordering and lets the
 * router fail-fast if a slot is left NULL).
 *
 * Debug-only views are guarded by MOKYA_DEBUG_VIEWS so production
 * builds skip them; the designated initializer pattern keeps the
 * array indexes correct regardless of which entries the preprocessor
 * removes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "view_router.h"

#include "boot_home_view.h"
#include "launcher_view.h"
#include "chat_list_view.h"
#include "conversation_view.h"
#include "tools_view.h"
#include "keypad_view.h"
#include "ime_view.h"
#include "nodes_view.h"
#include "node_detail_view.h"
#include "node_ops_view.h"
#include "my_node_view.h"
#include "settings/settings_app_view.h"
#if MOKYA_DEBUG_VIEWS
#include "rf_debug_view.h"
#include "font_test_view.h"
#endif

const view_descriptor_t *g_view_registry[VIEW_ID_COUNT];

void view_registry_populate(void)
{
    g_view_registry[VIEW_ID_BOOT_HOME]      = boot_home_view_descriptor();
    g_view_registry[VIEW_ID_LAUNCHER]       = launcher_view_descriptor();
    g_view_registry[VIEW_ID_MESSAGES]       = chat_list_view_descriptor();
    g_view_registry[VIEW_ID_MESSAGES_CHAT]  = conversation_view_descriptor();
    g_view_registry[VIEW_ID_NODES]          = nodes_view_descriptor();
    g_view_registry[VIEW_ID_NODE_DETAIL]    = node_detail_view_descriptor();
    g_view_registry[VIEW_ID_NODE_OPS]       = node_ops_view_descriptor();
    g_view_registry[VIEW_ID_MY_NODE]        = my_node_view_descriptor();
    g_view_registry[VIEW_ID_SETTINGS]       = settings_app_view_descriptor();
    g_view_registry[VIEW_ID_TOOLS]          = tools_view_descriptor();
    g_view_registry[VIEW_ID_IME]            = ime_view_descriptor();
    g_view_registry[VIEW_ID_KEYPAD]         = keypad_view_descriptor();
#if MOKYA_DEBUG_VIEWS
    g_view_registry[VIEW_ID_RF_DEBUG]       = rf_debug_view_descriptor();
    g_view_registry[VIEW_ID_FONT_TEST]      = font_test_view_descriptor();
#endif
}
