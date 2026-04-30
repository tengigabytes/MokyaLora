/* global_alert.c — see global_alert.h.
 *
 * Tick-driven orchestration of two notification sources on top of the
 * existing status_bar single-line alert overlay. No new widget tree —
 * we drive `status_bar_show_alert` / `status_bar_clear_alert` from a
 * polled view of dm_store + bq25622.
 *
 * Priority: a latched low-battery alert is sticky and outranks DM
 * toasts. We only fire DM toasts when the low-batt latch is clear, so
 * the user always sees the more urgent state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "global_alert.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "status_bar.h"
#include "dm_store.h"
#include "bq25622.h"
#include "view_router.h"
#include "chat_list_view.h"
#include "phoneapi_cache.h"
#include "node_alias.h"
#include "mokya_trace.h"

/* ── Thresholds ──────────────────────────────────────────────────────── */

/* Low-battery hysteresis. Linear-map SoC (status_bar.c:217) maps
 * 3300..4200 mV → 0..100 %, so 3500 mV ≈ 22 % and 3700 mV ≈ 44 %.
 * The wide gap avoids alert flapping while a charger pulses the rail. */
#define LOW_BATT_TRIP_MV     3500u
#define LOW_BATT_CLEAR_MV    3700u
#define LOW_BATT_POLL_MS     5000u   /* ~0.2 Hz — battery moves slowly  */

/* Real LiPo cells never drop below ~2500 mV — anything under this means
 * no battery installed (BAT pin floating). Don't latch alerts in that
 * case; would mask USB-only dev runs as "low battery". */
#define VBAT_PRESENT_MIN_MV  500u

/* DM toast cadence. Show for 4 s; new inbound supersedes any in-flight
 * info-level alert (status_bar handles single-slot replacement). */
#define DM_TOAST_DURATION_MS 4000u
#define DM_TOAST_POLL_MS     200u    /* ~5 Hz — match status_bar cadence */

/* Preview length. Status-bar alert label is 320 px / 16 px font ≈ 40
 * chars at single-byte; we cap at ~60 to leave room for the "Msg X: "
 * prefix. UTF-8 is byte-clipped — the worst case shows a truncated
 * trailing glyph, acceptable for a transient toast. */
#define DM_PREVIEW_BYTES_MAX  60u

/* ── State ───────────────────────────────────────────────────────────── */

typedef struct {
    /* DM toast bookkeeping */
    uint32_t last_seen_change_seq;
    uint32_t last_seen_unread_total;
    uint32_t next_dm_poll_ms;

    /* Low-battery hysteresis */
    bool     low_batt_latched;
    uint32_t next_batt_poll_ms;
} global_alert_t;

static global_alert_t s;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* True when the user's current focus is the conversation panel for
 * `peer`. Suppresses DM toasts the user is actively reading. */
static bool user_already_on_chat(uint32_t peer)
{
    if (view_router_active() != VIEW_ID_MESSAGES_CHAT) return false;
    return chat_list_get_active_peer() == peer;
}

/* Find the peer with the freshest inbound activity that still has unread
 * messages. dm_store_peer_at(0, ...) is most-recent-first — fits. */
static bool pick_toast_peer(uint32_t *peer_out)
{
    uint32_t n = dm_store_peer_count();
    for (uint32_t i = 0; i < n; ++i) {
        dm_peer_summary_t pp;
        if (!dm_store_peer_at(i, &pp)) continue;
        if (pp.unread == 0u) continue;
        *peer_out = pp.peer_node_id;
        return true;
    }
    return false;
}

/* Look up the freshest inbound text for `peer` (skip outbound entries
 * — we don't want to "toast" our own send). */
static bool pick_latest_inbound(uint32_t peer, dm_msg_t *msg_out)
{
    dm_peer_summary_t pp;
    if (!dm_store_get_peer(peer, &pp)) return false;
    if (pp.count == 0u) return false;
    /* idx 0 = oldest in ring; walk newest-first and pick the first
     * inbound. Common case (just-arrived DM) hits on the first try. */
    for (int i = (int)pp.count - 1; i >= 0; --i) {
        dm_msg_t m;
        if (!dm_store_get_msg(peer, (uint8_t)i, &m)) continue;
        if (!m.outbound) {
            *msg_out = m;
            return true;
        }
    }
    return false;
}

/* Compose the toast text. Uses node_alias → short_name → "!hex" lookup
 * chain (same as conversation_view header). */
static void format_toast(uint32_t peer, const dm_msg_t *m,
                         char *out, size_t cap)
{
    char nm[24];
    const char *short_name = NULL;
    phoneapi_node_t e;
    if (phoneapi_cache_get_node_by_id(peer, &e)) {
        short_name = e.short_name;
    }
    node_alias_format_display(peer, short_name, nm, sizeof(nm));

    /* Byte-clip preview at DM_PREVIEW_BYTES_MAX. UTF-8 mid-rune cut is
     * acceptable for a 4-second transient — the message is also visible
     * full-fidelity in the chat. */
    size_t preview_len = m->text_len;
    if (preview_len > DM_PREVIEW_BYTES_MAX) preview_len = DM_PREVIEW_BYTES_MAX;
    char preview[DM_PREVIEW_BYTES_MAX + 4];
    memcpy(preview, m->text, preview_len);
    if (m->text_len > DM_PREVIEW_BYTES_MAX) {
        preview[preview_len++] = '.';
        preview[preview_len++] = '.';
        preview[preview_len++] = '.';
    }
    preview[preview_len] = '\0';

    snprintf(out, cap, "Msg %s: %s", nm, preview);
}

/* ── DM toast tick ───────────────────────────────────────────────────── */

static void poll_dm_toast(uint32_t now)
{
    if (now < s.next_dm_poll_ms) return;
    s.next_dm_poll_ms = now + DM_TOAST_POLL_MS;

    /* Don't drown a sticky low-batt critical alert. */
    if (s.low_batt_latched) return;

    uint32_t cur_seq    = dm_store_change_seq();
    uint32_t cur_unread = dm_store_total_unread();

    /* Fast path: nothing changed. */
    if (cur_seq == s.last_seen_change_seq) return;

    /* change_seq bumps on outbound + ack updates too. Toast only when
     * the unread count actually rose — that's the "new inbound" signal. */
    if (cur_unread <= s.last_seen_unread_total) {
        s.last_seen_change_seq = cur_seq;
        return;
    }

    uint32_t peer = 0;
    if (!pick_toast_peer(&peer)) {
        s.last_seen_change_seq    = cur_seq;
        s.last_seen_unread_total  = cur_unread;
        return;
    }

    /* Suppress when the user is staring at this exact chat — they
     * already see the new bubble. */
    if (user_already_on_chat(peer)) {
        s.last_seen_change_seq    = cur_seq;
        s.last_seen_unread_total  = cur_unread;
        return;
    }

    dm_msg_t latest;
    if (!pick_latest_inbound(peer, &latest)) {
        s.last_seen_change_seq    = cur_seq;
        s.last_seen_unread_total  = cur_unread;
        return;
    }

    char buf[96];
    format_toast(peer, &latest, buf, sizeof(buf));
    status_bar_show_alert(/*level=info*/0, buf, DM_TOAST_DURATION_MS);
    TRACE("galert", "dm_toast", "peer=%lu unread=%lu",
          (unsigned long)peer, (unsigned long)cur_unread);

    s.last_seen_change_seq    = cur_seq;
    s.last_seen_unread_total  = cur_unread;
}

/* ── Low-battery tick ────────────────────────────────────────────────── */

static void poll_low_batt(uint32_t now)
{
    if (now < s.next_batt_poll_ms) return;
    s.next_batt_poll_ms = now + LOW_BATT_POLL_MS;

    const bq25622_state_t *b = bq25622_get_state();
    if (b == NULL || !b->online) return;
    uint32_t mv = b->vbat_mv;

    /* No battery installed — clear any latch and skip. */
    if (mv < VBAT_PRESENT_MIN_MV) {
        if (s.low_batt_latched) {
            s.low_batt_latched = false;
            status_bar_clear_alert();
            TRACE("galert", "low_batt_clr", "no_batt");
        }
        return;
    }

    if (!s.low_batt_latched && mv < LOW_BATT_TRIP_MV) {
        s.low_batt_latched = true;
        status_bar_show_alert(/*level=critical*/2,
                              "Low battery — charge soon", 0);
        TRACE("galert", "low_batt_set", "mv=%lu", (unsigned long)mv);
    } else if (s.low_batt_latched && mv > LOW_BATT_CLEAR_MV) {
        s.low_batt_latched = false;
        status_bar_clear_alert();
        TRACE("galert", "low_batt_clr", "mv=%lu", (unsigned long)mv);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void global_alert_init(void)
{
    memset(&s, 0, sizeof(s));
    /* Seed the unread baseline so a cascade replay populating dm_store
     * pre-toast (for messages we already saw before reboot) doesn't
     * surface as a "new" inbound on first tick. */
    s.last_seen_change_seq    = dm_store_change_seq();
    s.last_seen_unread_total  = dm_store_total_unread();
}

void global_alert_tick(void)
{
    uint32_t now = now_ms();
    poll_low_batt(now);     /* Critical first: it can latch the slot. */
    poll_dm_toast(now);
}
