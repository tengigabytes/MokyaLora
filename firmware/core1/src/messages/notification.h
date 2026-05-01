/* notification.h — central event router for the alert / vibration / LED
 *                    feedback system.
 *
 * Three-layer override resolution (narrow → wide):
 *   1. per-conversation override (matched on node_num, up to 8 entries)
 *   2. per-channel override (PHONEAPI_CHANNEL_COUNT, 8 channels)
 *   3. global event default (NOTIF_EVENT_*)
 *
 * The first layer that has a defined mode wins. Layers are "skipped"
 * when their valid bit is clear → fall through to the next layer.
 *
 * Mode is one of three states:
 *   NOTIF_MODE_NONE    — no actuator, no UI breadcrumb
 *   NOTIF_MODE_SILENT  — LED + status_bar pulse, no vibration
 *   NOTIF_MODE_VIBRATE — LED + vibration pattern
 *
 * DnD downgrade (if dnd_enable && wall-clock inside DnD window):
 *   VIBRATE → SILENT
 *   SILENT  → NONE
 *   SOS / LOW_BATT events bypass DnD.
 *
 * Master-enable=0 forces every event to NONE (still logs the call so
 * UI counters stay accurate).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_NOTIFICATION_H
#define MOKYA_CORE1_NOTIFICATION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Public types ──────────────────────────────────────────────────── */

typedef enum {
    NOTIF_MODE_NONE    = 0,
    NOTIF_MODE_SILENT  = 1,
    NOTIF_MODE_VIBRATE = 2,
    NOTIF_MODE_DEFAULT = 3,     /* override-only sentinel: clears the
                                 * override and falls through. Never
                                 * stored in event_modes / channel_modes
                                 * directly. */
} notif_mode_t;

typedef enum {
    NOTIF_EVENT_DM = 0,
    NOTIF_EVENT_BROADCAST,
    NOTIF_EVENT_ACK,
    NOTIF_EVENT_NEW_NODE,
    NOTIF_EVENT_CHARGE,
    NOTIF_EVENT_LOW_BATT,
    NOTIF_EVENT_KEYPRESS,
    NOTIF_EVENT_SOS,
    NOTIF_EVENT_COUNT
} notif_event_t;

#define NOTIF_CHANNEL_COUNT  8u
#define NOTIF_PER_CONV_MAX   8u
#define NOTIF_SETTINGS_VERSION 0x02u    /* v2: split DnD + Quiet Hours */

typedef struct {
    uint32_t node_num;          /* 0 = empty slot */
    uint8_t  mode;              /* notif_mode_t (NONE/SILENT/VIBRATE) */
    uint8_t  reserved[3];
} notif_per_conv_t;

typedef struct {
    uint8_t  version;             /* NOTIF_SETTINGS_VERSION */
    uint8_t  master_enable;       /* 0 = global mute */
    uint8_t  dnd_enable;          /* manual instant toggle (no time window) */
    uint8_t  vib_intensity;       /* 0..100 */
    uint8_t  quiet_hours_enable;  /* scheduled quiet hours */
    uint8_t  reserved1[3];
    uint16_t quiet_start_min;     /* 0..1439 (HH*60+MM) */
    uint16_t quiet_end_min;
    /* 8 events × 2 bits each = 16 bits. bit[2n+1:2n] = event_modes[n]. */
    uint16_t event_modes;
    /* Channel override valid mask (bit n = channel n has override). */
    uint8_t  channel_valid_mask;
    uint8_t  reserved2[3];
    /* 8 channels × 2 bits each. */
    uint16_t channel_modes;
    uint16_t reserved3;
    /* 8 conversation overrides × 8 B each = 64 B. */
    notif_per_conv_t per_conv[NOTIF_PER_CONV_MAX];
} notif_settings_t;               /* sizeof ≈ 84 B */

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Apply factory defaults. Idempotent. */
void notif_settings_set_defaults(notif_settings_t *s);

/* Initialise the runtime; loads settings from notif_persist if
 * available, else applies defaults. Safe to call once at boot. */
void notification_init(void);

/* ── Settings accessors ────────────────────────────────────────────── */

/* Return a pointer to the live settings block. Read/write — caller
 * must invoke notification_settings_dirty() after mutating. */
notif_settings_t *notification_get_settings(void);

/* Flag that the settings have been mutated; persistence layer will
 * pick this up on its next debounce tick. */
void notification_settings_dirty(void);

/* Clear the dirty flag. Used by notif_persist after a successful save. */
bool notification_settings_consume_dirty(void);

/* Pack/unpack helpers for the 2-bit fields. */
notif_mode_t notif_event_mode_get(const notif_settings_t *s,
                                  notif_event_t ev);
void         notif_event_mode_set(notif_settings_t *s,
                                  notif_event_t ev,
                                  notif_mode_t mode);
notif_mode_t notif_channel_mode_get(const notif_settings_t *s,
                                    uint8_t channel_idx);
/* mode = NOTIF_MODE_DEFAULT clears the override. */
void         notif_channel_mode_set(notif_settings_t *s,
                                    uint8_t channel_idx,
                                    notif_mode_t mode);

/* Per-conversation overrides. Upsert returns the slot index used or
 * 0xFF if full. mode = NOTIF_MODE_DEFAULT removes the entry. */
uint8_t      notif_per_conv_upsert(notif_settings_t *s,
                                   uint32_t node_num,
                                   notif_mode_t mode);
notif_mode_t notif_per_conv_get(const notif_settings_t *s,
                                uint32_t node_num);

/* ── Resolver + dispatch ───────────────────────────────────────────── */

/* Pure resolver — three-layer lookup + DnD downgrade + master-enable.
 * channel_idx = 0xFF if not applicable; node_num = 0 if not applicable.
 * Returns the final mode that would actuate. */
notif_mode_t notif_resolve(notif_event_t ev,
                           uint8_t channel_idx,
                           uint32_t node_num);

/* Fire an event. Internally resolves and drives the actuators. */
void notification_event(notif_event_t ev,
                        uint8_t channel_idx,
                        uint32_t node_num);

/* SWD-readable last-resolve result (for tests). Updated on every
 * notification_event call. */
typedef struct {
    uint32_t calls_total;
    uint32_t event_last;
    uint32_t channel_last;
    uint32_t node_last;
    uint32_t mode_last;             /* notif_mode_t after resolve+gate */
    uint32_t suppressed_dnd;
    uint32_t suppressed_master;
} notif_diag_t;

const notif_diag_t *notification_get_diag(void);

#ifdef __cplusplus
}
#endif
#endif /* MOKYA_CORE1_NOTIFICATION_H */
