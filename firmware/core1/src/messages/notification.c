/* notification.c — see notification.h. */

#include "notification.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "vib_motor.h"
#include "lm27965.h"
#include "wall_clock.h"

/* ── Settings instance ─────────────────────────────────────────────── */

/* Live settings — kept in regular .bss (~76 B) so SWD reads are
 * coherent. PSRAM would be cache-aliased on the SWD side per
 * project_psram_swd_cache_coherence; the size is small enough to
 * justify the SRAM cost. */
notif_settings_t g_notif_settings;
#define s_settings g_notif_settings
static volatile bool    s_dirty     __attribute__((section(".psram_bss")));
static volatile bool    s_inited    __attribute__((section(".psram_bss")));

/* Quiet-hours wall clock comes from the wall_clock module. When the
 * clock isn't synced (g_wc_synced == 0) Quiet Hours never triggers,
 * matching "沒同步不顯示時間" — UI shows "—:—". */

static notif_diag_t  s_diag         __attribute__((section(".psram_bss")));

/* SWD-readable mirrors of the most common state. */
volatile uint32_t g_notif_calls_total    __attribute__((used)) = 0u;
volatile uint8_t  g_notif_mode_last      __attribute__((used)) = 0u;
volatile uint8_t  g_notif_event_last     __attribute__((used)) = 0xFFu;
volatile uint8_t  g_notif_master_enable  __attribute__((used)) = 1u;
volatile uint8_t  g_notif_dirty_flag     __attribute__((used)) = 0u;

/* ── Defaults ──────────────────────────────────────────────────────── */

void notif_settings_set_defaults(notif_settings_t *s)
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    s->version             = NOTIF_SETTINGS_VERSION;
    s->master_enable       = 1;
    s->dnd_enable          = 0;
    s->quiet_hours_enable  = 0;
    s->vib_intensity       = 70;
    s->quiet_start_min     = 22 * 60;     /* 22:00 */
    s->quiet_end_min       =  7 * 60;     /* 07:00 */

    notif_event_mode_set(s, NOTIF_EVENT_DM,        NOTIF_MODE_VIBRATE);
    notif_event_mode_set(s, NOTIF_EVENT_BROADCAST, NOTIF_MODE_SILENT);
    notif_event_mode_set(s, NOTIF_EVENT_ACK,       NOTIF_MODE_NONE);
    notif_event_mode_set(s, NOTIF_EVENT_NEW_NODE,  NOTIF_MODE_NONE);
    notif_event_mode_set(s, NOTIF_EVENT_CHARGE,    NOTIF_MODE_NONE);
    notif_event_mode_set(s, NOTIF_EVENT_LOW_BATT,  NOTIF_MODE_SILENT);
    notif_event_mode_set(s, NOTIF_EVENT_KEYPRESS,  NOTIF_MODE_NONE);
    notif_event_mode_set(s, NOTIF_EVENT_SOS,       NOTIF_MODE_VIBRATE);

    s->channel_valid_mask = 0u;
    s->channel_modes      = 0u;
}

void notification_init(void)
{
    if (s_inited) return;
    notif_settings_set_defaults(&s_settings);
    /* notif_persist_load() will overwrite on its own init pass; we
     * stay with defaults until then. */
    s_dirty = false;
    g_notif_master_enable = s_settings.master_enable;
    s_inited = true;
}

/* ── Settings accessors ────────────────────────────────────────────── */

notif_settings_t *notification_get_settings(void)
{
    if (!s_inited) notification_init();
    return &s_settings;
}

void notification_settings_dirty(void)
{
    s_dirty = true;
    g_notif_dirty_flag = 1u;
    g_notif_master_enable = s_settings.master_enable;
}

bool notification_settings_consume_dirty(void)
{
    bool was = s_dirty;
    s_dirty = false;
    g_notif_dirty_flag = 0u;
    return was;
}

notif_mode_t notif_event_mode_get(const notif_settings_t *s, notif_event_t ev)
{
    if (s == NULL || (unsigned)ev >= NOTIF_EVENT_COUNT) return NOTIF_MODE_NONE;
    return (notif_mode_t)((s->event_modes >> (2u * ev)) & 0x3u);
}

void notif_event_mode_set(notif_settings_t *s, notif_event_t ev,
                          notif_mode_t mode)
{
    if (s == NULL || (unsigned)ev >= NOTIF_EVENT_COUNT) return;
    if (mode == NOTIF_MODE_DEFAULT) mode = NOTIF_MODE_NONE;
    uint16_t shift = (uint16_t)(2u * ev);
    s->event_modes &= (uint16_t)~(0x3u << shift);
    s->event_modes |= (uint16_t)((mode & 0x3u) << shift);
}

notif_mode_t notif_channel_mode_get(const notif_settings_t *s, uint8_t ch)
{
    if (s == NULL || ch >= NOTIF_CHANNEL_COUNT) return NOTIF_MODE_DEFAULT;
    if ((s->channel_valid_mask & (1u << ch)) == 0u) return NOTIF_MODE_DEFAULT;
    return (notif_mode_t)((s->channel_modes >> (2u * ch)) & 0x3u);
}

void notif_channel_mode_set(notif_settings_t *s, uint8_t ch, notif_mode_t mode)
{
    if (s == NULL || ch >= NOTIF_CHANNEL_COUNT) return;
    if (mode == NOTIF_MODE_DEFAULT) {
        s->channel_valid_mask &= (uint8_t)~(1u << ch);
        return;
    }
    uint16_t shift = (uint16_t)(2u * ch);
    s->channel_modes &= (uint16_t)~(0x3u << shift);
    s->channel_modes |= (uint16_t)((mode & 0x3u) << shift);
    s->channel_valid_mask |= (uint8_t)(1u << ch);
}

uint8_t notif_per_conv_upsert(notif_settings_t *s, uint32_t node_num,
                              notif_mode_t mode)
{
    if (s == NULL || node_num == 0u) return 0xFFu;
    /* Search for existing entry first. */
    for (uint8_t i = 0; i < NOTIF_PER_CONV_MAX; i++) {
        if (s->per_conv[i].node_num == node_num) {
            if (mode == NOTIF_MODE_DEFAULT) {
                s->per_conv[i].node_num = 0u;
                s->per_conv[i].mode = 0u;
                return i;
            }
            s->per_conv[i].mode = (uint8_t)mode;
            return i;
        }
    }
    if (mode == NOTIF_MODE_DEFAULT) return 0xFFu;
    /* Place into first empty slot. */
    for (uint8_t i = 0; i < NOTIF_PER_CONV_MAX; i++) {
        if (s->per_conv[i].node_num == 0u) {
            s->per_conv[i].node_num = node_num;
            s->per_conv[i].mode     = (uint8_t)mode;
            return i;
        }
    }
    return 0xFFu;
}

notif_mode_t notif_per_conv_get(const notif_settings_t *s, uint32_t node_num)
{
    if (s == NULL || node_num == 0u) return NOTIF_MODE_DEFAULT;
    for (uint8_t i = 0; i < NOTIF_PER_CONV_MAX; i++) {
        if (s->per_conv[i].node_num == node_num) {
            return (notif_mode_t)s->per_conv[i].mode;
        }
    }
    return NOTIF_MODE_DEFAULT;
}

/* ── Suppression helpers ──────────────────────────────────────────── *
 *
 * Two independent gates can downgrade a notification:
 *
 *   1. dnd_enable      — manual "mute now" toggle. No time window;
 *                        flip on/off as needed.
 *   2. quiet_hours     — scheduled time window. Only meaningful when
 *                        the wall clock has been synced.
 *
 * Either gate, when active and the event isn't SOS / LOW_BATT, drops
 * the resolved mode by one step (VIB→SIL, SIL→NONE). The two gates
 * compose: hitting both is the same as hitting either (single-step
 * drop, not two — UX choice; double-drop is too aggressive). */

static bool quiet_hours_active(const notif_settings_t *s)
{
    if (!s->quiet_hours_enable) return false;
    uint16_t now = wall_clock_minute_of_day();
    if (now == 0xFFFFu) return false;        /* unsynced */
    uint16_t a = s->quiet_start_min;
    uint16_t b = s->quiet_end_min;
    if (a == b) return false;
    if (a < b) return (now >= a) && (now < b);
    /* wraps midnight */
    return (now >= a) || (now < b);
}

static bool suppression_active(const notif_settings_t *s)
{
    return s->dnd_enable || quiet_hours_active(s);
}

static notif_mode_t mode_downgrade(notif_mode_t m)
{
    if (m == NOTIF_MODE_VIBRATE) return NOTIF_MODE_SILENT;
    if (m == NOTIF_MODE_SILENT)  return NOTIF_MODE_NONE;
    return m;
}

static notif_mode_t apply_gates(const notif_settings_t *s, notif_event_t ev,
                                notif_mode_t m)
{
    if (ev == NOTIF_EVENT_SOS || ev == NOTIF_EVENT_LOW_BATT) return m;
    if (suppression_active(s)) return mode_downgrade(m);
    return m;
}

/* ── Resolver ──────────────────────────────────────────────────────── */

notif_mode_t notif_resolve(notif_event_t ev, uint8_t ch_idx, uint32_t node_num)
{
    if (!s_inited) notification_init();
    const notif_settings_t *s = &s_settings;

    /* Layer 1: per-conversation. */
    if (node_num != 0u) {
        notif_mode_t m = notif_per_conv_get(s, node_num);
        if (m != NOTIF_MODE_DEFAULT) return apply_gates(s, ev, m);
    }

    /* Layer 2: per-channel — only meaningful for DM/BROADCAST. */
    if (ch_idx < NOTIF_CHANNEL_COUNT &&
        (ev == NOTIF_EVENT_DM || ev == NOTIF_EVENT_BROADCAST)) {
        notif_mode_t m = notif_channel_mode_get(s, ch_idx);
        if (m != NOTIF_MODE_DEFAULT) return apply_gates(s, ev, m);
    }

    /* Layer 3: global event default. */
    return apply_gates(s, ev, notif_event_mode_get(s, ev));
}

/* ── Actuator dispatch ─────────────────────────────────────────────── */

static vib_preset_t preset_for_event(notif_event_t ev)
{
    switch (ev) {
        case NOTIF_EVENT_DM:        return VIB_PRESET_DOUBLE;
        case NOTIF_EVENT_BROADCAST: return VIB_PRESET_TRIPLE;
        case NOTIF_EVENT_ACK:       return VIB_PRESET_SHORT;
        case NOTIF_EVENT_NEW_NODE:  return VIB_PRESET_SHORT;
        case NOTIF_EVENT_CHARGE:    return VIB_PRESET_TICK;
        case NOTIF_EVENT_LOW_BATT:  return VIB_PRESET_DOUBLE;
        case NOTIF_EVENT_KEYPRESS:  return VIB_PRESET_TICK;
        case NOTIF_EVENT_SOS:       return VIB_PRESET_LONG;
        default:                    return VIB_PRESET_TICK;
    }
}

void notification_event(notif_event_t ev, uint8_t ch_idx, uint32_t node_num)
{
    if (!s_inited) notification_init();

    s_diag.calls_total++;
    s_diag.event_last   = (uint32_t)ev;
    s_diag.channel_last = (uint32_t)ch_idx;
    s_diag.node_last    = node_num;
    g_notif_calls_total = s_diag.calls_total;
    g_notif_event_last  = (uint8_t)ev;

    if (!s_settings.master_enable) {
        s_diag.suppressed_master++;
        s_diag.mode_last = NOTIF_MODE_NONE;
        g_notif_mode_last = NOTIF_MODE_NONE;
        return;
    }

    notif_mode_t mode = notif_resolve(ev, ch_idx, node_num);
    s_diag.mode_last = (uint32_t)mode;
    g_notif_mode_last = (uint8_t)mode;

    switch (mode) {
        case NOTIF_MODE_NONE:
            return;
        case NOTIF_MODE_SILENT:
            /* LED-only: red for DM/SOS, green for everything else. */
            if (ev == NOTIF_EVENT_SOS || ev == NOTIF_EVENT_LOW_BATT) {
                lm27965_set_led_red(true, LM27965_DUTY_C_MAX);
            } else if (ev == NOTIF_EVENT_DM) {
                lm27965_set_led_red(true, 1);
            } else {
                lm27965_set_keypad_backlight(false, true,
                                              LM27965_DUTY_AB_MAX / 4u);
            }
            return;
        case NOTIF_MODE_VIBRATE:
            vib_play_preset(preset_for_event(ev), s_settings.vib_intensity);
            /* Mirror to LED so a glance confirms the alert source. */
            if (ev == NOTIF_EVENT_SOS || ev == NOTIF_EVENT_LOW_BATT) {
                lm27965_set_led_red(true, LM27965_DUTY_C_MAX);
            } else if (ev == NOTIF_EVENT_DM) {
                lm27965_set_led_red(true, 1);
            } else {
                lm27965_set_keypad_backlight(false, true,
                                              LM27965_DUTY_AB_MAX / 4u);
            }
            return;
        default:
            return;
    }
}

const notif_diag_t *notification_get_diag(void)
{
    return &s_diag;
}
