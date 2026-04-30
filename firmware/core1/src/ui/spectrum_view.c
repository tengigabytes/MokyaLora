/* spectrum_view.c — see spectrum_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header   "T-3 訊號頻譜  N peers"
 *   y  16..207  up to 8 rows × 24 px (1 row per peer, sorted by SNR desc)
 *   y 208..223  hint     "BACK 工具"
 *
 * Row format:
 *   `<short_name>  ±SS.S  [########]  Nh  <age>`
 *
 * Bar cells (8 wide) map SNR x100 onto -10..+10 dB linearly:
 *   snr_x100 ≤ -1000  → 0 cells
 *   snr_x100 ≥ +1000  → 8 cells
 *   else proportional, clamped.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "spectrum_view.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "phoneapi_cache.h"
#include "node_alias.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define ROW_H           24
#define MAX_ROWS         8
#define HINT_H          16
#define PANEL_W        320
#define BAR_CELLS        8

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[MAX_ROWS];
    lv_obj_t *hint;
    uint32_t  last_change_seq;
    uint32_t  last_refresh_ms;
} spec_t;

static spec_t s;

/* SWD diagnostic exports — published on every render() so a host
 * test can verify peer-counting + sort logic against CLI ground
 * truth. Memory cost: 12 B. */
volatile uint32_t g_t3_collected      __attribute__((used)) = 0u;
volatile int32_t  g_t3_top_snr_x100   __attribute__((used)) = INT32_MIN;
volatile uint32_t g_t3_top_node_num   __attribute__((used)) = 0u;

typedef struct {
    uint32_t node_num;
    char     name[16];
    int32_t  snr_x100;
    uint8_t  hops_away;
    uint32_t last_heard;
} peer_row_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

static int snr_to_cells(int32_t snr_x100)
{
    if (snr_x100 <= -1000) return 0;
    if (snr_x100 >=  1000) return BAR_CELLS;
    /* Map -1000..+1000 → 0..BAR_CELLS linearly. */
    return (int)((snr_x100 + 1000) * BAR_CELLS / 2000);
}

static void format_age(uint32_t age_secs, char *buf, size_t cap)
{
    if (age_secs < 60u)        snprintf(buf, cap, "%us",   (unsigned)age_secs);
    else if (age_secs < 3600u) snprintf(buf, cap, "%um",   (unsigned)(age_secs / 60u));
    else if (age_secs < 86400u)snprintf(buf, cap, "%uh",   (unsigned)(age_secs / 3600u));
    else                       snprintf(buf, cap, "%ud",   (unsigned)(age_secs / 86400u));
}

static int peer_row_compare_desc(const void *a, const void *b)
{
    /* Sort by SNR descending (strongest first). INT32_MIN sentinel
     * sorts to the bottom. */
    const peer_row_t *pa = (const peer_row_t *)a;
    const peer_row_t *pb = (const peer_row_t *)b;
    if (pa->snr_x100 == pb->snr_x100) return 0;
    return (pa->snr_x100 < pb->snr_x100) ? 1 : -1;
}

static void clear_rows(void)
{
    for (int i = 0; i < MAX_ROWS; ++i) {
        if (s.rows[i]) lv_label_set_text(s.rows[i], "");
    }
}

static void render(void)
{
    /* Collect non-self peers with valid SNR. */
    phoneapi_my_info_t mi;
    bool have_mi = phoneapi_cache_get_my_info(&mi);
    uint32_t total = phoneapi_cache_node_count();

    peer_row_t rows[PHONEAPI_NODES_CAP];
    uint32_t collected = 0u;

    for (uint32_t i = 0; i < total && collected < PHONEAPI_NODES_CAP; ++i) {
        phoneapi_node_t e;
        if (!phoneapi_cache_take_node_at(i, &e)) continue;
        if (have_mi && e.num == mi.my_node_num) continue;
        if (e.snr_x100 == INT32_MIN) continue;          /* skip unknown */
        rows[collected].node_num   = e.num;
        rows[collected].snr_x100   = e.snr_x100;
        rows[collected].hops_away  = e.hops_away;
        rows[collected].last_heard = e.last_heard;
        node_alias_format_display(e.num, e.short_name,
                                   rows[collected].name,
                                   sizeof(rows[collected].name));
        collected++;
    }

    /* Sort by SNR desc. Bubble sort — n ≤ 32, fine for occasional tick. */
    for (uint32_t i = 0; i + 1u < collected; ++i) {
        for (uint32_t j = 0; j + 1u < collected - i; ++j) {
            if (peer_row_compare_desc(&rows[j], &rows[j + 1u]) > 0) {
                peer_row_t tmp = rows[j];
                rows[j] = rows[j + 1u];
                rows[j + 1u] = tmp;
            }
        }
    }

    /* Wall-clock ish for "heard age" — same trick as F-3, take the
     * newest last_heard across cache as reference. */
    uint32_t now_epoch = 0u;
    for (uint32_t i = 0; i < collected; ++i) {
        if (rows[i].last_heard > now_epoch) now_epoch = rows[i].last_heard;
    }

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "T-3 訊號頻譜  %lu peers (SNR known)",
             (unsigned long)collected);
    lv_label_set_text(s.header, hdr);

    clear_rows();
    uint32_t shown = collected < MAX_ROWS ? collected : MAX_ROWS;
    for (uint32_t i = 0; i < shown; ++i) {
        peer_row_t *r = &rows[i];
        int cells = snr_to_cells(r->snr_x100);
        char bar[BAR_CELLS + 3];
        bar[0] = '[';
        for (int c = 0; c < BAR_CELLS; ++c) {
            bar[1 + c] = (c < cells) ? '#' : '.';
        }
        bar[1 + BAR_CELLS] = ']';
        bar[2 + BAR_CELLS] = '\0';

        char snr_s[10];
        int snr_int = r->snr_x100 / 100;
        int snr_dec = (r->snr_x100 < 0 ? -r->snr_x100 : r->snr_x100) % 100 / 10;
        snprintf(snr_s, sizeof(snr_s), "%+d.%01d", snr_int, snr_dec);

        char hop_s[6];
        if (r->hops_away == 0xFFu) snprintf(hop_s, sizeof(hop_s), "--");
        else                       snprintf(hop_s, sizeof(hop_s), "%uh", (unsigned)r->hops_away);

        char age_s[8] = "--";
        if (now_epoch != 0u && r->last_heard != 0u && r->last_heard <= now_epoch) {
            format_age(now_epoch - r->last_heard, age_s, sizeof(age_s));
        }

        char buf[80];
        snprintf(buf, sizeof(buf), "%-9s %-5s %s %-3s %s",
                 r->name, snr_s, bar, hop_s, age_s);
        lv_label_set_text(s.rows[i], buf);
    }

    if (collected == 0u) {
        lv_label_set_text(s.rows[0],
            "(no SNR data — cascade hasn't seen any peer yet)");
    }

    /* SWD diag exports — capture top peer for verification gate. */
    g_t3_collected = collected;
    if (collected > 0u) {
        g_t3_top_snr_x100 = rows[0].snr_x100;
        g_t3_top_node_num = rows[0].node_num;
    } else {
        g_t3_top_snr_x100 = INT32_MIN;
        g_t3_top_node_num = 0u;
    }
    TRACE("spec", "render", "collected=%lu", (unsigned long)collected);
}

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    for (int i = 0; i < MAX_ROWS; ++i) {
        s.rows[i] = make_label(panel, 4, HEADER_H + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }
    s.hint = make_label(panel, 4, HEADER_H + MAX_ROWS * ROW_H,
                        PANEL_W - 8, HINT_H,
                        ui_color(UI_COLOR_TEXT_SECONDARY));
    lv_label_set_text(s.hint, "BACK 工具");

    render();
}

static void destroy(void)
{
    s.header = NULL;
    for (int i = 0; i < MAX_ROWS; ++i) s.rows[i] = NULL;
    s.hint = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    if (ev->keycode == MOKYA_KEY_BACK) {
        view_router_navigate(VIEW_ID_TOOLS);
    }
}

static void refresh(void)
{
    if (s.header == NULL) return;
    /* Re-render on cache mutation; otherwise once a second so the
     * "heard age" column doesn't freeze. */
    uint32_t cur = phoneapi_cache_change_seq();
    uint32_t t   = now_ms();
    if (cur == s.last_change_seq && (t - s.last_refresh_ms) < 1000u) return;
    s.last_change_seq = cur;
    s.last_refresh_ms = t;
    render();
}

static const view_descriptor_t SPECTRUM_DESC = {
    .id      = VIEW_ID_T3_SPECTRUM,
    .name    = "spectrum",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { NULL, NULL, "BACK 工具" },
};

const view_descriptor_t *spectrum_view_descriptor(void)
{
    return &SPECTRUM_DESC;
}
