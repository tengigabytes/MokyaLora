/* sys_diag_view.c — see sys_diag_view.h.
 *
 * Pages 1-3 land in this file as separate static handlers (mirrors the
 * hw_diag_view pattern). Per-page state lives in PSRAM .bss to free
 * SRAM budget; SWD diag mirrors live in regular .bss.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_diag_view.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"

#include "cpu_load.h"
#include "msp_canary.h"
#include "c1_storage.h"

/* ── Page registry ────────────────────────────────────────────────── */

typedef enum {
    SYS_PAGE_RESOURCES = 0,
    SYS_PAGE_CPU,
    SYS_PAGE_SCREEN,
    SYS_PAGE_COUNT
} sys_page_t;

typedef struct {
    const char *name;
    void (*enter)(lv_obj_t *root);
    void (*leave)(void);
    void (*apply)(const key_event_t *ev);
    void (*refresh)(void);
} sys_page_def_t;

static void resources_enter(lv_obj_t *root);
static void resources_leave(void);
static void resources_refresh(void);
static void cpu_enter(lv_obj_t *root);
static void cpu_leave(void);
static void cpu_refresh(void);
static void screen_enter(lv_obj_t *root);
static void screen_leave(void);
static void screen_apply(const key_event_t *ev);
static void screen_refresh(void);

static sys_page_def_t s_pages[SYS_PAGE_COUNT] = {
    [SYS_PAGE_RESOURCES] = { "資源",     resources_enter, resources_leave,
                             NULL, resources_refresh },
    [SYS_PAGE_CPU]       = { "CPU+任務", cpu_enter,       cpu_leave,
                             NULL, cpu_refresh },
    [SYS_PAGE_SCREEN]    = { "螢幕",     screen_enter,    screen_leave,
                             screen_apply, screen_refresh },
};

/* ── View state ───────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t  *bg;
    lv_obj_t  *title_lbl;
    lv_obj_t  *content_root;
    sys_page_t cur_page;
} sys_diag_state_t;

static sys_diag_state_t s __attribute__((section(".psram_bss")));

/* Single packed status u32:
 *   bits  0..7:  cur_page (0..2)
 *   bits  8..15: page-context-dependent —
 *                  cur_page=CPU      → task_count
 *                  cur_page=SCREEN   → screen_state byte
 *                                        bit 0  fps_overlay_on
 *                                        bit 1  mode (0=normal, 1=pixtest)
 *                                        bits 2..4  pixtest_idx (0..4)
 *   bits 16..31: page-context-dependent —
 *                  cur_page=RESOURCES → heap_free / 4
 *                                       (52 KB / 4 fits in 14 bits)
 *                  cur_page=SCREEN    → fps_value × 10 (e.g. 300 = 30.0 FPS)
 *
 * Test reads after navigating to specific page, so context byte is
 * always valid for the page being inspected. Single-word mirror keeps
 * SRAM .bss footprint to 4 B. */
volatile uint32_t g_sys_diag_status __attribute__((used)) = 0u;

/* ── Helpers ──────────────────────────────────────────────────────── */

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_font(lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(lbl, ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    return lbl;
}

static void update_title(void)
{
    if (s.title_lbl == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "[系統診斷] ◀ %u/%u %s ▶",
             (unsigned)(s.cur_page + 1u),
             (unsigned)SYS_PAGE_COUNT,
             s_pages[s.cur_page].name);
    lv_label_set_text(s.title_lbl, buf);
}

static void clear_content(void)
{
    if (s.content_root == NULL) return;
    lv_obj_clean(s.content_root);
}

static void switch_page(sys_page_t to)
{
    if (to >= SYS_PAGE_COUNT) return;
    if (s_pages[s.cur_page].leave) s_pages[s.cur_page].leave();
    clear_content();
    s.cur_page = to;
    g_sys_diag_status = (g_sys_diag_status & 0xFFFFFF00u) | (uint32_t)to;
    update_title();
    if (s_pages[s.cur_page].enter) s_pages[s.cur_page].enter(s.content_root);
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.title_lbl = lv_label_create(panel);
    lv_obj_set_pos(s.title_lbl, 4, 2);
    lv_obj_set_size(s.title_lbl, 312, 16);
    lv_obj_set_style_text_font(s.title_lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.title_lbl,
                                ui_color(UI_COLOR_ACCENT_FOCUS), 0);
    lv_obj_set_style_pad_all(s.title_lbl, 0, 0);
    lv_label_set_long_mode(s.title_lbl, LV_LABEL_LONG_CLIP);

    s.content_root = lv_obj_create(panel);
    lv_obj_set_pos(s.content_root, 0, 20);
    lv_obj_set_size(s.content_root, 320, 204);
    lv_obj_set_style_bg_opa(s.content_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.content_root, 0, 0);
    lv_obj_set_style_radius(s.content_root, 0, 0);
    lv_obj_set_style_pad_all(s.content_root, 0, 0);
    lv_obj_clear_flag(s.content_root, LV_OBJ_FLAG_SCROLLABLE);

    s.cur_page = SYS_PAGE_RESOURCES;
    g_sys_diag_status = (g_sys_diag_status & 0xFFFFFF00u) | (uint32_t)s.cur_page;
    update_title();
    if (s_pages[s.cur_page].enter) s_pages[s.cur_page].enter(s.content_root);
}

static void destroy(void)
{
    if (s_pages[s.cur_page].leave) s_pages[s.cur_page].leave();
    memset(&s, 0, sizeof(s));
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_LEFT:
            switch_page((sys_page_t)((s.cur_page + SYS_PAGE_COUNT - 1u) % SYS_PAGE_COUNT));
            break;
        case MOKYA_KEY_RIGHT:
            switch_page((sys_page_t)((s.cur_page + 1u) % SYS_PAGE_COUNT));
            break;
        default:
            if (s_pages[s.cur_page].apply) s_pages[s.cur_page].apply(ev);
            break;
    }
}

static void refresh(void)
{
    if (s_pages[s.cur_page].refresh) s_pages[s.cur_page].refresh();
}

static const view_descriptor_t SYS_DIAG_DESC = {
    .id      = VIEW_ID_SYS_DIAG,
    .name    = "sys_diag",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "◀▶ 切換頁", "OK 互動", "BACK 返回" },
};

const view_descriptor_t *sys_diag_view_descriptor(void)
{
    return &SYS_DIAG_DESC;
}

/* ── Page: Resources (SYS_PAGE_RESOURCES) ─────────────────────────── *
 *
 * Static usage (linker symbols) plus dynamic state (heap free / minimum
 * ever, LFS used). Refresh once per second — these change slowly. */

extern char __bss_start__[];
extern char __bss_end__[];
extern char __data_start__[];
extern char __data_end__[];
extern char __HeapLimit[];
extern char __StackTop[];
extern char __flash_binary_end[];

static struct {
    lv_obj_t *heap_lbl;
    lv_obj_t *sram_lbl;
    lv_obj_t *psram_lbl;
    lv_obj_t *flash_lbl;
    lv_obj_t *lfs_lbl;
    lv_obj_t *msp_lbl;
    lv_obj_t *uptime_lbl;
    uint32_t  last_tick;
} s_res __attribute__((section(".psram_bss")));

static void resources_enter(lv_obj_t *root)
{
    s_res.heap_lbl   = mk_label(root, 4,   0);
    s_res.sram_lbl   = mk_label(root, 4,  16);
    s_res.psram_lbl  = mk_label(root, 4,  32);
    s_res.flash_lbl  = mk_label(root, 4,  48);
    s_res.lfs_lbl    = mk_label(root, 4,  64);
    s_res.msp_lbl    = mk_label(root, 4,  80);
    s_res.uptime_lbl = mk_label(root, 4,  96);
    s_res.last_tick = 0;
    resources_refresh();
}

static void resources_leave(void) { memset(&s_res, 0, sizeof(s_res)); }

static void resources_refresh(void)
{
    if (s_res.heap_lbl == NULL) return;
    uint32_t tick = lv_tick_get();
    if ((tick - s_res.last_tick) < 1000u) return;
    s_res.last_tick = tick;

    char buf[80];

    /* FreeRTOS heap. */
    size_t heap_total = configTOTAL_HEAP_SIZE;
    size_t heap_free  = xPortGetFreeHeapSize();
    size_t heap_min   = xPortGetMinimumEverFreeHeapSize();
    /* Pack heap_free / 4 into bits 16..31 of g_sys_diag_status. */
    uint32_t hf_words = ((uint32_t)heap_free / 4u) & 0xFFFFu;
    g_sys_diag_status = (g_sys_diag_status & 0x0000FFFFu) | (hf_words << 16);
    (void)heap_min;
    snprintf(buf, sizeof(buf), "FreeRTOS heap %u/%u  最低剩 %u B",
             (unsigned)heap_free, (unsigned)heap_total, (unsigned)heap_min);
    lv_label_set_text(s_res.heap_lbl, buf);

    /* SRAM static. */
    size_t bss   = (size_t)(__bss_end__ - __bss_start__);
    size_t data  = (size_t)(__data_end__ - __data_start__);
    snprintf(buf, sizeof(buf), "SRAM .bss %u  .data %u  總 312KB",
             (unsigned)bss, (unsigned)data);
    lv_label_set_text(s_res.sram_lbl, buf);

    /* PSRAM — .psram_bss section size. */
    extern char __psram_bss_start[] __attribute__((weak));
    extern char __psram_bss_end[]   __attribute__((weak));
    size_t psram_used = 0;
    if (__psram_bss_end != NULL && __psram_bss_start != NULL) {
        psram_used = (size_t)(__psram_bss_end - __psram_bss_start);
    }
    snprintf(buf, sizeof(buf), "PSRAM .bss %u 由 1024 KB", (unsigned)psram_used);
    lv_label_set_text(s_res.psram_lbl, buf);

    /* Flash image. */
    uintptr_t img_end = (uintptr_t)__flash_binary_end;
    size_t img_bytes = (img_end > 0x10200000u) ? (img_end - 0x10200000u) : 0;
    snprintf(buf, sizeof(buf), "Flash image %u KB / 2048 KB",
             (unsigned)(img_bytes / 1024u));
    lv_label_set_text(s_res.flash_lbl, buf);

    /* LFS storage. */
    c1_storage_stats_t stats;
    c1_storage_get_stats(&stats);
    /* lfs_used / lfs_total mirrored into existing g_c1_storage_blocks_*
     * by c1_storage_get_stats; nothing to do here. */
    if (stats.mounted && stats.blocks_total > 0) {
        unsigned pct = (unsigned)(stats.blocks_used * 100u / stats.blocks_total);
        snprintf(buf, sizeof(buf), "LFS %u/%u blocks (%u%%)  schema v%u",
                 (unsigned)stats.blocks_used,
                 (unsigned)stats.blocks_total,
                 pct,
                 (unsigned)stats.schema_version);
    } else {
        snprintf(buf, sizeof(buf), "LFS unmounted");
    }
    lv_label_set_text(s_res.lfs_lbl, buf);

    /* MSP high water + stack region. */
    msp_canary_refresh();
    uint32_t msp_peak = msp_canary_peak_used();
    snprintf(buf, sizeof(buf), "MSP 用量峰值 %u B  (2048 B 保留)",
             (unsigned)msp_peak);
    lv_label_set_text(s_res.msp_lbl, buf);

    /* Uptime. */
    uint32_t up_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t s_ = up_ms / 1000u;
    snprintf(buf, sizeof(buf), "Uptime %lu:%02lu:%02lu",
             (unsigned long)(s_ / 3600u),
             (unsigned long)((s_ / 60u) % 60u),
             (unsigned long)(s_ % 60u));
    lv_label_set_text(s_res.uptime_lbl, buf);
}

/* ── Page: CPU + Tasks (SYS_PAGE_CPU) ─────────────────────────────── *
 *
 * Core 1 busy% derived by debug/cpu_load.c (idle-hook delta). Core 0
 * shown as "需要 IPC, v2"  — adding it requires a Meshtastic submodule
 * patch + Core 0 1 Hz publish task; deferred. Top 6 tasks by stack
 * high-water from uxTaskGetSystemState. */

#define CPU_TASK_ROWS  6

static struct {
    lv_obj_t *cpu1_lbl;
    lv_obj_t *cpu0_lbl;
    lv_obj_t *task_hdr_lbl;
    lv_obj_t *task_lbls[CPU_TASK_ROWS];
    lv_obj_t *task_count_lbl;
    uint32_t  last_tick;
} s_cpu __attribute__((section(".psram_bss")));

static void cpu_enter(lv_obj_t *root)
{
    s_cpu.cpu1_lbl = mk_label(root, 4,   0);
    s_cpu.cpu0_lbl = mk_label(root, 4,  16);
    s_cpu.task_hdr_lbl = mk_label(root, 4, 38);
    lv_label_set_text(s_cpu.task_hdr_lbl, "Task              prio  hwm");
    for (int i = 0; i < CPU_TASK_ROWS; i++) {
        s_cpu.task_lbls[i] = mk_label(root, 4, 54 + i * 16);
    }
    s_cpu.task_count_lbl = mk_label(root, 4, 54 + CPU_TASK_ROWS * 16 + 4);
    s_cpu.last_tick = 0;
    cpu_refresh();
}

static void cpu_leave(void) { memset(&s_cpu, 0, sizeof(s_cpu)); }

static int task_cmp_by_hwm(const void *a, const void *b)
{
    const TaskStatus_t *ta = (const TaskStatus_t *)a;
    const TaskStatus_t *tb = (const TaskStatus_t *)b;
    /* Smallest hwm = closest to overflow → list first. */
    if (ta->usStackHighWaterMark < tb->usStackHighWaterMark) return -1;
    if (ta->usStackHighWaterMark > tb->usStackHighWaterMark) return 1;
    return 0;
}

static void cpu_refresh(void)
{
    if (s_cpu.cpu1_lbl == NULL) return;
    uint32_t tick = lv_tick_get();
    if ((tick - s_cpu.last_tick) < 1000u) return;
    s_cpu.last_tick = tick;

    char buf[80];

    /* Core 1 CPU%. After ~5 windows with no idle progression, declare
     * "idle starved" — usb_device_task / bridge_task tight taskYIELD
     * loops at priority+2 prevent the priority-0 idle task from ever
     * running, so the idle-hook based metric collapses. Keep showing
     * windows count as proof of life. */
    uint8_t inst = cpu_load_pct_instant();
    uint8_t avg  = cpu_load_pct_avg10();
    uint32_t windows = cpu_load_window_count();
    if (windows == 0u) {
        snprintf(buf, sizeof(buf), "Core 1 CPU  -- %% (waiting first window)");
    } else if (g_cpu_idle_baseline == 0u && windows >= 5u) {
        snprintf(buf, sizeof(buf),
                 "Core 1 CPU  ~100%% (idle 飢餓 — windows=%lu)",
                 (unsigned long)windows);
    } else if (inst == UINT8_MAX) {
        snprintf(buf, sizeof(buf), "Core 1 CPU  -- %% (校準中, windows=%lu)",
                 (unsigned long)windows);
    } else if (avg == UINT8_MAX) {
        snprintf(buf, sizeof(buf), "Core 1 CPU  %u %% (10s avg 待累積)",
                 (unsigned)inst);
    } else {
        snprintf(buf, sizeof(buf), "Core 1 CPU  %u %%   (10s avg %u %%)",
                 (unsigned)inst, (unsigned)avg);
    }
    lv_label_set_text(s_cpu.cpu1_lbl, buf);

    lv_label_set_text(s_cpu.cpu0_lbl,
                      "Core 0 CPU  --  (需要 IPC stats — v2)");

    /* Task enumeration. uxTaskGetSystemState fills a TaskStatus_t array.
     * Allocate from FreeRTOS heap so we don't blow stack. */
    UBaseType_t total = uxTaskGetNumberOfTasks();
    if (total == 0u || total > 32u) {
        for (int i = 0; i < CPU_TASK_ROWS; i++) {
            lv_label_set_text(s_cpu.task_lbls[i], "");
        }
        snprintf(buf, sizeof(buf), "tasks: %u (out of range)", (unsigned)total);
        lv_label_set_text(s_cpu.task_count_lbl, buf);
        return;
    }
    TaskStatus_t *snapshot = pvPortMalloc(total * sizeof(TaskStatus_t));
    if (snapshot == NULL) {
        for (int i = 0; i < CPU_TASK_ROWS; i++) {
            lv_label_set_text(s_cpu.task_lbls[i], "");
        }
        lv_label_set_text(s_cpu.task_count_lbl, "tasks: 失敗 (heap?)");
        return;
    }
    UBaseType_t got = uxTaskGetSystemState(snapshot, total, NULL);
    /* Pack task_count into bits 8..15 of g_sys_diag_status. */
    uint32_t tc = (uint32_t)(got & 0xFFu) << 8;
    g_sys_diag_status = (g_sys_diag_status & 0xFFFF00FFu) | tc;

    /* Sort ascending by stack high-water (smallest first = riskiest). */
    for (UBaseType_t i = 1; i < got; i++) {
        TaskStatus_t key = snapshot[i];
        int j = (int)i - 1;
        while (j >= 0 && task_cmp_by_hwm(&snapshot[j], &key) > 0) {
            snapshot[j + 1] = snapshot[j];
            j--;
        }
        snapshot[j + 1] = key;
    }

    for (int i = 0; i < CPU_TASK_ROWS; i++) {
        if ((UBaseType_t)i < got) {
            const char *name = snapshot[i].pcTaskName ? snapshot[i].pcTaskName : "?";
            snprintf(buf, sizeof(buf), "%-14s %2u  %4u",
                     name,
                     (unsigned)snapshot[i].uxCurrentPriority,
                     (unsigned)snapshot[i].usStackHighWaterMark);
            lv_label_set_text(s_cpu.task_lbls[i], buf);
        } else {
            lv_label_set_text(s_cpu.task_lbls[i], "");
        }
    }
    snprintf(buf, sizeof(buf), "Total %u tasks  (hwm = words 剩餘)",
             (unsigned)got);
    lv_label_set_text(s_cpu.task_count_lbl, buf);
    vPortFree(snapshot);
}

/* ── Page: Screen test (SYS_PAGE_SCREEN) ──────────────────────────── *
 *
 * Two functions:
 *   1. FPS overlay toggle — OK toggles a corner label that updates with
 *      the moving-average LVGL frame rate (via lv_disp's render counter
 *      we approximate via lv_tick_get diffs).
 *   2. Pixel test — UP enters fullscreen-fill mode, OK cycles through
 *      R/G/B/W/K. BACK exits both modes back to the launcher (handled
 *      by view_router as usual). */

typedef enum {
    SCREEN_MODE_NORMAL  = 0,
    SCREEN_MODE_PIXTEST = 1,
} screen_mode_t;

static const lv_color_t k_pixtest_colors[5] = {
    /* R, G, B, W, K. */
    LV_COLOR_MAKE(0xFF, 0x00, 0x00),
    LV_COLOR_MAKE(0x00, 0xFF, 0x00),
    LV_COLOR_MAKE(0x00, 0x00, 0xFF),
    LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
    LV_COLOR_MAKE(0x00, 0x00, 0x00),
};
static const char *k_pixtest_names[5] = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };

static struct {
    lv_obj_t *info_lbl;        /* "FPS overlay: ON/OFF  (OK 切換)" etc. */
    lv_obj_t *fps_lbl;         /* corner overlay label */
    lv_obj_t *mode_hint_lbl;   /* "↑ 進入像素測試..." or "OK 換顏色 / ↑ 退出" */
    lv_obj_t *fill_obj;        /* fullscreen-fill rect (only in pixtest mode) */
    lv_obj_t *fill_lbl;        /* color name in pixtest */
    bool      fps_overlay_on;
    screen_mode_t mode;
    uint8_t   pixtest_idx;
    uint32_t  last_tick;
    uint32_t  last_fps_tick;
    uint32_t  fps_count;       /* refreshes since last calc */
    uint16_t  fps_value_x10;   /* current FPS × 10 */
} s_scr __attribute__((section(".psram_bss")));

static void update_fps_overlay(void)
{
    if (s_scr.fps_lbl == NULL) return;
    if (!s_scr.fps_overlay_on) {
        lv_obj_add_flag(s_scr.fps_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_scr.fps_lbl, LV_OBJ_FLAG_HIDDEN);
    char buf[24];
    snprintf(buf, sizeof(buf), "%u.%u FPS",
             (unsigned)(s_scr.fps_value_x10 / 10u),
             (unsigned)(s_scr.fps_value_x10 % 10u));
    lv_label_set_text(s_scr.fps_lbl, buf);
}

static void show_normal_mode(lv_obj_t *root)
{
    if (s_scr.fill_obj) { lv_obj_del(s_scr.fill_obj); s_scr.fill_obj = NULL; }
    if (s_scr.fill_lbl) { lv_obj_del(s_scr.fill_lbl); s_scr.fill_lbl = NULL; }

    if (s_scr.info_lbl == NULL) {
        s_scr.info_lbl      = mk_label(root, 4, 0);
        s_scr.mode_hint_lbl = mk_label(root, 4, 24);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "FPS overlay: %s   (OK 切換)",
             s_scr.fps_overlay_on ? "ON" : "OFF");
    lv_label_set_text(s_scr.info_lbl, buf);
    lv_label_set_text(s_scr.mode_hint_lbl, "↑ 進入像素測試 (R/G/B/W/K)");
}

static void show_pixtest_mode(lv_obj_t *root)
{
    if (s_scr.info_lbl)      { lv_obj_add_flag(s_scr.info_lbl, LV_OBJ_FLAG_HIDDEN); }
    if (s_scr.mode_hint_lbl) { lv_obj_add_flag(s_scr.mode_hint_lbl, LV_OBJ_FLAG_HIDDEN); }

    if (s_scr.fill_obj == NULL) {
        s_scr.fill_obj = lv_obj_create(root);
        lv_obj_set_pos(s_scr.fill_obj, 0, 0);
        lv_obj_set_size(s_scr.fill_obj, 320, 204);
        lv_obj_set_style_bg_opa(s_scr.fill_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_scr.fill_obj, 0, 0);
        lv_obj_set_style_radius(s_scr.fill_obj, 0, 0);
        lv_obj_set_style_pad_all(s_scr.fill_obj, 0, 0);
        lv_obj_clear_flag(s_scr.fill_obj, LV_OBJ_FLAG_SCROLLABLE);

        s_scr.fill_lbl = lv_label_create(s_scr.fill_obj);
        lv_obj_set_style_text_font(s_scr.fill_lbl, ui_font_sm16(), 0);
        lv_obj_align(s_scr.fill_lbl, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    }
    /* Apply current color. */
    lv_obj_set_style_bg_color(s_scr.fill_obj,
                              k_pixtest_colors[s_scr.pixtest_idx], 0);
    /* Pick contrasting label color (white on dark, black on light). */
    bool is_light = (s_scr.pixtest_idx == 3);   /* WHITE */
    lv_obj_set_style_text_color(s_scr.fill_lbl,
                                is_light ? lv_color_hex(0x000000)
                                         : lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_scr.fill_lbl, k_pixtest_names[s_scr.pixtest_idx]);
}

static void screen_enter(lv_obj_t *root)
{
    /* FPS overlay label — created up-front, hidden until toggled on. */
    s_scr.fps_lbl = mk_label(root, 240, 0);
    lv_obj_set_style_text_color(s_scr.fps_lbl,
                                ui_color(UI_COLOR_ACCENT_FOCUS), 0);
    lv_obj_add_flag(s_scr.fps_lbl, LV_OBJ_FLAG_HIDDEN);

    s_scr.mode = SCREEN_MODE_NORMAL;
    show_normal_mode(root);
    update_fps_overlay();
    s_scr.last_tick     = lv_tick_get();
    s_scr.last_fps_tick = s_scr.last_tick;
    s_scr.fps_count     = 0;
    /* Reset screen state byte but keep cur_page intact. */
    g_sys_diag_status &= 0xFFFF00FFu;
}

static void screen_leave(void) { memset(&s_scr, 0, sizeof(s_scr)); }

static void screen_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    if (s_scr.mode == SCREEN_MODE_NORMAL) {
        if (ev->keycode == MOKYA_KEY_OK) {
            s_scr.fps_overlay_on = !s_scr.fps_overlay_on;
            update_fps_overlay();
            show_normal_mode(s.content_root);
        } else if (ev->keycode == MOKYA_KEY_UP) {
            s_scr.mode = SCREEN_MODE_PIXTEST;
            s_scr.pixtest_idx = 0;
            show_pixtest_mode(s.content_root);
        }
    } else {
        /* Pixtest mode. */
        if (ev->keycode == MOKYA_KEY_OK) {
            s_scr.pixtest_idx = (uint8_t)((s_scr.pixtest_idx + 1u) % 5u);
            show_pixtest_mode(s.content_root);
        } else if (ev->keycode == MOKYA_KEY_UP) {
            s_scr.mode = SCREEN_MODE_NORMAL;
            show_normal_mode(s.content_root);
        }
    }
    /* Mirror screen state into bits 8..15 of g_sys_diag_status. */
    uint32_t state_byte = (s_scr.fps_overlay_on ? 1u : 0u)
                        | ((uint32_t)s_scr.mode << 1)
                        | ((uint32_t)(s_scr.pixtest_idx & 0x07u) << 2);
    g_sys_diag_status = (g_sys_diag_status & 0xFFFF00FFu)
                      | (state_byte << 8);
}

static void screen_refresh(void)
{
    if (s_scr.fps_lbl == NULL) return;
    /* refresh callback fires every lvgl_task tick; count those + recompute
     * the FPS over a 1-second window. lv_disp_get_default()'s internal
     * stats would be more accurate but require an LVGL-private include. */
    s_scr.fps_count++;
    uint32_t tick = lv_tick_get();
    uint32_t dt = tick - s_scr.last_fps_tick;
    if (dt >= 1000u) {
        /* fps × 10 = count * 10000 / dt_ms */
        s_scr.fps_value_x10 = (uint16_t)((uint32_t)s_scr.fps_count * 10000u / dt);
        s_scr.fps_count = 0;
        s_scr.last_fps_tick = tick;
        if (s_scr.fps_overlay_on) update_fps_overlay();
        /* Mirror fps_value × 10 into bits 16..31 of g_sys_diag_status
         * (only when this page is active — context-dependent slot). */
        g_sys_diag_status = (g_sys_diag_status & 0x0000FFFFu)
                          | ((uint32_t)s_scr.fps_value_x10 << 16);
    }
}
