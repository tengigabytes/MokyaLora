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
#include "message_detail_view.h"
#include "canned_view.h"
#include "tools_view.h"
#include "keypad_view.h"
#include "ime_view.h"
#include "nodes_view.h"
#include "node_detail_view.h"
#include "node_ops_view.h"
#include "remote_admin_view.h"
#include "my_node_view.h"
#include "settings/settings_app_view.h"
#include "modules_index_view.h"
#include "telemetry_view.h"
#include "map_view.h"
#include "map_nav_view.h"
#include "channels_view.h"
#include "channel_edit_view.h"
#include "channel_add_view.h"
#include "channel_share_view.h"
#include "pairing_view.h"
#include "spectrum_view.h"
#include "sniffer_view.h"
#include "lora_test_view.h"
#include "rhw_pins_view.h"
#include "rhw_pin_edit_view.h"
#include "waypoints_view.h"
#include "waypoint_detail_view.h"
#include "waypoint_edit_view.h"
#include "traceroute_view.h"
#include "range_test_view.h"
#include "gnss_sky_view.h"
#include "firmware_info_view.h"
#if MOKYA_DEBUG_VIEWS
#include "rf_debug_view.h"
#include "font_test_view.h"
#endif

/* Lookup table is populated once at boot (view_registry_populate) and
 * read-only afterwards. Place in PSRAM .bss to free SRAM for stack /
 * .bss budget. Cost is one PSRAM read per navigate, which is cheap
 * (write-back cached, sequential). */
const view_descriptor_t *g_view_registry[VIEW_ID_COUNT]
    __attribute__((section(".psram_bss")));

void view_registry_populate(void)
{
    g_view_registry[VIEW_ID_BOOT_HOME]      = boot_home_view_descriptor();
    g_view_registry[VIEW_ID_LAUNCHER]       = launcher_view_descriptor();
    g_view_registry[VIEW_ID_MESSAGES]       = chat_list_view_descriptor();
    g_view_registry[VIEW_ID_MESSAGES_CHAT]  = conversation_view_descriptor();
    g_view_registry[VIEW_ID_MESSAGE_DETAIL] = message_detail_view_descriptor();
    g_view_registry[VIEW_ID_CANNED]         = canned_view_descriptor();
    g_view_registry[VIEW_ID_NODES]          = nodes_view_descriptor();
    g_view_registry[VIEW_ID_NODE_DETAIL]    = node_detail_view_descriptor();
    g_view_registry[VIEW_ID_NODE_OPS]       = node_ops_view_descriptor();
    g_view_registry[VIEW_ID_REMOTE_ADMIN]   = remote_admin_view_descriptor();
    g_view_registry[VIEW_ID_MY_NODE]        = my_node_view_descriptor();
    g_view_registry[VIEW_ID_SETTINGS]       = settings_app_view_descriptor();
    g_view_registry[VIEW_ID_MODULES_INDEX]  = modules_index_view_descriptor();
    g_view_registry[VIEW_ID_TELEMETRY]      = telemetry_view_descriptor();
    g_view_registry[VIEW_ID_MAP]            = map_view_descriptor();
    g_view_registry[VIEW_ID_MAP_NAV]        = map_nav_view_descriptor();
    g_view_registry[VIEW_ID_CHANNELS]       = channels_view_descriptor();
    g_view_registry[VIEW_ID_CHANNEL_EDIT]   = channel_edit_view_descriptor();
    g_view_registry[VIEW_ID_TOOLS]          = tools_view_descriptor();
    g_view_registry[VIEW_ID_TRACEROUTE]     = traceroute_view_descriptor();
    g_view_registry[VIEW_ID_RANGE_TEST]     = range_test_view_descriptor();
    g_view_registry[VIEW_ID_GNSS_SKY]       = gnss_sky_view_descriptor();
    g_view_registry[VIEW_ID_FIRMWARE_INFO]  = firmware_info_view_descriptor();
    g_view_registry[VIEW_ID_IME]            = ime_view_descriptor();
    g_view_registry[VIEW_ID_KEYPAD]         = keypad_view_descriptor();
    g_view_registry[VIEW_ID_CHANNEL_ADD]    = channel_add_view_descriptor();
    g_view_registry[VIEW_ID_CHANNEL_SHARE]  = channel_share_view_descriptor();
    g_view_registry[VIEW_ID_T7_PAIRING]     = pairing_view_descriptor();
    g_view_registry[VIEW_ID_T3_SPECTRUM]    = spectrum_view_descriptor();
    g_view_registry[VIEW_ID_T4_SNIFFER]     = sniffer_view_descriptor();
    g_view_registry[VIEW_ID_T5_LORA_TEST]   = lora_test_view_descriptor();
    g_view_registry[VIEW_ID_T10_RHW_PINS]    = rhw_pins_view_descriptor();
    g_view_registry[VIEW_ID_T10_RHW_PIN_EDIT]= rhw_pin_edit_view_descriptor();
    g_view_registry[VIEW_ID_WAYPOINTS]       = waypoints_view_descriptor();
    g_view_registry[VIEW_ID_WAYPOINT_DETAIL] = waypoint_detail_view_descriptor();
    g_view_registry[VIEW_ID_WAYPOINT_EDIT]   = waypoint_edit_view_descriptor();
#if MOKYA_DEBUG_VIEWS
    g_view_registry[VIEW_ID_RF_DEBUG]       = rf_debug_view_descriptor();
    g_view_registry[VIEW_ID_FONT_TEST]      = font_test_view_descriptor();
#endif
}
