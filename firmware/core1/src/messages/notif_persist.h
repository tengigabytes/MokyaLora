/* notif_persist.h — LFS persistence for notification settings.
 *
 * Stored at "/notif.bin" (no subdirectory; matches dm_persist's
 * top-level convention).
 *
 * Save trigger: notification_settings_dirty() flips a flag; a 5 s
 * debounce timer drains and writes if set. 5 s smooths burst edits
 * (e.g. cycling through several event modes in the settings UI) into
 * one flash write.
 *
 * Load trigger: notif_persist_load() called from bridge_task setup
 * after c1_storage_init succeeds. Falls back to defaults on missing /
 * corrupt file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_NOTIF_PERSIST_H
#define MOKYA_CORE1_NOTIF_PERSIST_H

#include <stdbool.h>
#include <stdint.h>

#include "notification.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NOTIF_PERSIST_MAGIC   0x46544F4Eu   /* 'NOTF' little-endian */
#define NOTIF_PERSIST_VERSION 1u
#define NOTIF_PERSIST_PATH    "/notif.bin"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc32;            /* over the body bytes that follow */
    uint32_t reserved;         /* keep header 16 B aligned */
    notif_settings_t body;
} notif_persist_record_t;

/* Initialise the persistence layer + start the debounce flush timer.
 * Idempotent. Must run after c1_storage_init() and notification_init(). */
void notif_persist_init(void);

/* Force-flush the current settings synchronously. Returns true on
 * success or when nothing was dirty. */
bool notif_persist_flush_now(void);

/* Load and apply the saved settings into the live notification core.
 * Returns true on success. Defaults already in place are kept on
 * failure / missing file. */
bool notif_persist_load(void);

#ifdef __cplusplus
}
#endif
#endif /* MOKYA_CORE1_NOTIF_PERSIST_H */
