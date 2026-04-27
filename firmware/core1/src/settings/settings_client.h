/* settings_client.h — IPC config sender + reply matcher for settings_view.
 *
 * Producer: settings_view (LVGL task) calls settings_client_get() /
 *           settings_client_set() / settings_client_commit() to push
 *           IPC_CMD_GET_CONFIG / SET_CONFIG / COMMIT_CONFIG / COMMIT_REBOOT
 *           onto the c1_to_c0 ring.
 *
 * Consumer of replies: bridge_task (USB bridge task) calls
 *           settings_client_dispatch_reply() on every IPC_MSG_CONFIG_VALUE /
 *           IPC_MSG_CONFIG_RESULT it pops off c0_to_c1, which enqueues the
 *           reply onto an internal FreeRTOS queue. settings_view
 *           polls settings_client_take_reply() each LVGL refresh tick.
 *
 * Wire ordering matches the Core 0 handler:
 *   - GET_CONFIG → CONFIG_VALUE reply (or CONFIG_RESULT on unknown key)
 *   - SET_CONFIG → CONFIG_RESULT reply (OK / UNKNOWN_KEY / INVALID_VALUE)
 *   - COMMIT_CONFIG / COMMIT_REBOOT → CONFIG_RESULT reply (key=0)
 *
 * Reply matching is via the IPC seq byte: the caller does not see seq
 * directly, but every send returns true/false (push success). The view
 * is single-threaded against the user, so at any moment there is at
 * most one outstanding request — match by key (or by reply order for
 * commits). Queue depth 8 absorbs the GET burst at first activation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_CLIENT_VALUE_MAX  64u

typedef enum {
    SCR_VALUE         = 0,   ///< CONFIG_VALUE reply — value[] valid
    SCR_OK            = 1,   ///< CONFIG_RESULT result=OK
    SCR_UNKNOWN_KEY   = 2,
    SCR_INVALID_VALUE = 3,
    SCR_BUSY          = 4,
} settings_client_kind_t;

typedef struct {
    uint8_t  kind;       ///< settings_client_kind_t
    uint16_t key;        ///< IPC_CFG_* this reply pertains to (0 for commit results)
    uint16_t value_len;  ///< only valid when kind == SCR_VALUE
    uint8_t  value[SETTINGS_CLIENT_VALUE_MAX];
} settings_client_reply_t;

/** Create the reply queue. Call once before scheduler starts. */
void settings_client_init(void);

/* ── Senders (called from LVGL task) ───────────────────────────────────── */

bool settings_client_send_get(uint16_t ipc_key);
bool settings_client_send_set(uint16_t ipc_key,
                              const void *value, uint16_t value_len);
/** reboot=true → IPC_CMD_COMMIT_REBOOT, false → IPC_CMD_COMMIT_CONFIG */
bool settings_client_send_commit(bool reboot);

/* ── Reply intake (called from bridge_task) ───────────────────────────── */

/** Build a reply from an IPC_MSG_CONFIG_VALUE or _RESULT slot and enqueue. */
void settings_client_dispatch_reply(uint8_t msg_id,
                                    const uint8_t *payload,
                                    uint16_t payload_len);

/* ── Reply consumption (called from LVGL task) ────────────────────────── */

/** Non-blocking pop. Returns true if `out` was filled. */
bool settings_client_take_reply(settings_client_reply_t *out);

#ifdef __cplusplus
}
#endif
