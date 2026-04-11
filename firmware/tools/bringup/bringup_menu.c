// bringup_menu.c
// Interactive LCD menu system for MokyaLora bringup firmware.
//
// Displays a categorised menu on the ST7789VI TFT, navigated with the
// physical keypad (UP/DOWN/OK/BACK).  Serial commands always take priority
// to preserve bringup_run.ps1 automation compatibility.
//
// TFT drawing primitives live in bringup_tft_draw.c — this file contains
// only the menu data structures, page definitions, and navigation logic.

#include "bringup.h"
#include "bringup_menu.h"

// ---------------------------------------------------------------------------
// Menu layout constants (derived from TFT drawing getters)
// ---------------------------------------------------------------------------

#define MENU_SCALE     2
#define MENU_CELL_W   (6 * MENU_SCALE)   // 12 px
#define MENU_CELL_H   (8 * MENU_SCALE)   // 16 px
// Dynamic columns/rows based on current rotation
#define MENU_COLS     (menu_tft_width()  / MENU_CELL_W)
#define MENU_ROWS     (menu_tft_height() / MENU_CELL_H)
#define VISIBLE_ITEMS (MENU_ROWS - 4)     // title + sep + sep + hint = 4 rows

// ---------------------------------------------------------------------------
// Menu page definitions
// ---------------------------------------------------------------------------

// Thin wrappers for commands that need bus_b_init/deinit bracketing
static void cmd_charge_on(void)  { bus_b_init(); bq25622_enable_charge(); bus_b_deinit(); }
static void cmd_charge_off(void) { bus_b_init(); bq25622_disable_charge(); bus_b_deinit(); }
static void cmd_charge_scan(void) {
    bus_b_init();
    bq25622_enable_charge();
    sleep_ms(500);
    bus_b_deinit();
    perform_scan(i2c1, BUS_B_SDA, BUS_B_SCL, "Bus B (Power, i2c1) -- post charge_on");
    printf("Expected: 0x6B(Charger)  0x55(FuelGauge -- only if awake)  0x36(LED)\n");
}
static void cmd_scan_b_tft(void) {
    scan_bus_b();
    while (!back_key_pressed()) sleep_ms(50);
}
static void cmd_charger_diag(void) {
    charger_diag();
}
static void cmd_gauge_diag(void) {
    gauge_diag();
}
static void cmd_led(void) {
    bus_b_init();
    led_control();
    bus_b_deinit();
}
static void cmd_scan_a(void) {
    scan_bus_a();
    // Keep results visible until BACK key (back_key_init already
    // called by menu_handle_key before invoking run callback).
    while (!back_key_pressed()) sleep_ms(50);
}
static void cmd_lora_test(void) {
    lora_test();
    while (!back_key_pressed()) sleep_ms(50);
}
static void cmd_lora_rx_lf(void) {
    lora_rx(920125000UL, 11, 0x05, 0x01, 30);
}
static void cmd_lora_rx_mf(void) {
    lora_rx(922125000UL, 9, 0x05, 0x01, 0);
}
static void cmd_lora_rx_mf1(void) {
    lora_rx(920125000UL, 9, 0x05, 0x01, 0);
}
static void cmd_lora_tx(void) {
    lora_tx();
    while (!back_key_pressed()) sleep_ms(50);
}
static void cmd_lora_dump(void) {
    lora_dump();
    while (!back_key_pressed()) sleep_ms(50);
}

// --- Sensors page ---
static const menu_item_t page_sensors_items[] = {
    {"IMU read",       imu_read,     NULL},
    {"Baro read",      baro_read,    NULL},
    {"Mag read",       mag_read,     NULL},
    {"NMEA polling",   gnss_info,    NULL},
    {"GNSS TFT live",  gnss_tft_test, NULL},
    {"GNSS RF debug",  gnss_rftft,   NULL},
    {"Bus A Diag",     cmd_scan_a,   NULL},
};
static const menu_page_t page_sensors = {
    "Sensors", page_sensors_items, sizeof(page_sensors_items) / sizeof(page_sensors_items[0])
};

// --- Power page ---
static const menu_item_t page_power_items[] = {
    {"Bus B Diag",      cmd_scan_b_tft,  NULL},
    {"Charger Diag",    cmd_charger_diag,NULL},
    {"Gauge Diag",      cmd_gauge_diag,  NULL},
    {"Charge ON",       cmd_charge_on,   NULL},
    {"Charge OFF",      cmd_charge_off,  NULL},
    {"Charge scan",     cmd_charge_scan, NULL},
    {"LED control",     cmd_led,         NULL},
    {"Motor breathe",   motor_breathe,   NULL},
};
static const menu_page_t page_power = {
    "Power", page_power_items, sizeof(page_power_items) / sizeof(page_power_items[0])
};

// --- Audio page ---
static const menu_item_t page_audio_items[] = {
    {"Amp test",       amp_test,     NULL},
    {"Amp breathe",    amp_breathe,  NULL},
    {"Bee melody",     amp_bee,      NULL},
    {"Mic test",       mic_test,     NULL},
    {"Mic raw 10s",    mic_raw,      NULL},
    {"Mic loopback",   mic_loopback, NULL},
    {"Mic record",     mic_rec,      NULL},
    {"Mic dump",       mic_dump,     NULL},
};
static const menu_page_t page_audio = {
    "Audio", page_audio_items, sizeof(page_audio_items) / sizeof(page_audio_items[0])
};

// --- Memory page ---
static const menu_item_t page_memory_items[] = {
    {"Memory Diag",    cmd_memory_diag,    NULL},
    {"PSRAM full 8MB", cmd_psram_full_tft, NULL},
    {"PSRAM Speed",    cmd_psram_dma_test, NULL},
    {"PSRAM Tuning",   cmd_psram_tuning,   NULL},
    {"PSRAM Debug",    cmd_psram_debug,    NULL},
};
static const menu_page_t page_memory = {
    "Memory", page_memory_items, sizeof(page_memory_items) / sizeof(page_memory_items[0])
};

// --- LoRa page ---
static const menu_item_t page_lora_items[] = {
    {"LoRa test",       cmd_lora_test,   NULL},
    {"LoRa RX 30s LF",  cmd_lora_rx_lf, NULL},
    {"LoRa RX MF",      cmd_lora_rx_mf,  NULL},
    {"LoRa RX MF1",     cmd_lora_rx_mf1, NULL},
    {"LoRa TX",         cmd_lora_tx,     NULL},
    {"LoRa dump",       cmd_lora_dump,   NULL},
};
static const menu_page_t page_lora = {
    "LoRa", page_lora_items, sizeof(page_lora_items) / sizeof(page_lora_items[0])
};

// Rotation wrapper — cycles 0->90->180->270 and redraws menu
static void cmd_rotate(void) {
    menu_tft_rotate();
}

// --- Display page ---
static const menu_item_t page_display_items[] = {
    {"TFT test",       tft_test,        NULL},
    {"TFT fast",       tft_fast_test,   NULL},
    {"Rotate screen",  cmd_rotate,      NULL},
};
static const menu_page_t page_display = {
    "Display", page_display_items, sizeof(page_display_items) / sizeof(page_display_items[0])
};

// --- Keyboard page ---
static const menu_item_t page_keyboard_items[] = {
    {"Key monitor",    key_monitor,     NULL},
    {"Key TFT test",   key_tft_test,    NULL},
};
static const menu_page_t page_keyboard = {
    "Keyboard", page_keyboard_items, sizeof(page_keyboard_items) / sizeof(page_keyboard_items[0])
};

// --- Core 1 page ---
static const menu_item_t page_core1_items[] = {
    {"Core1 test",     core1_test,      NULL},
};
static const menu_page_t page_core1 = {
    "Core 1", page_core1_items, sizeof(page_core1_items) / sizeof(page_core1_items[0])
};

// --- Root menu ---
static const menu_item_t root_items[] = {
    {"Sensors",   NULL, &page_sensors},
    {"Power",     NULL, &page_power},
    {"Audio",     NULL, &page_audio},
    {"Memory",    NULL, &page_memory},
    {"LoRa",      NULL, &page_lora},
    {"Display",   NULL, &page_display},
    {"Keyboard",  NULL, &page_keyboard},
    {"Core 1",    NULL, &page_core1},
};
static const menu_page_t root_page = {
    "MokyaLora Bringup", root_items, sizeof(root_items) / sizeof(root_items[0])
};

const menu_page_t *menu_root_page(void) { return &root_page; }

// ---------------------------------------------------------------------------
// Keyboard scanning for navigation
// ---------------------------------------------------------------------------

// Navigation key positions in the 6x6 matrix:
//   UP    = R0C5    DOWN  = R2C5    LEFT  = R0C2
//   RIGHT = R3C5    OK    = R1C5    BACK  = R0C1

menu_key_t menu_scan_key(void) {
    static uint32_t last_event_ms = 0;
    static uint8_t  prev_state = 0;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_event_ms < 80) return KEY_NONE;  // debounce

    uint8_t pressed[KEY_ROWS];
    key_scan_matrix(pressed);

    // Pack nav key states into a bitmask
    uint8_t state = 0;
    if (pressed[0] & (1u << 5)) state |= (1u << 0); // UP
    if (pressed[2] & (1u << 5)) state |= (1u << 1); // DOWN
    if (pressed[0] & (1u << 2)) state |= (1u << 2); // LEFT
    if (pressed[3] & (1u << 5)) state |= (1u << 3); // RIGHT
    if (pressed[1] & (1u << 5)) state |= (1u << 4); // OK
    if (pressed[0] & (1u << 1)) state |= (1u << 5); // BACK

    uint8_t newly = state & ~prev_state;  // rising edges only
    prev_state = state;

    if (!newly) return KEY_NONE;
    last_event_ms = now;

    // Priority: BACK > OK > UP > DOWN > LEFT > RIGHT
    if (newly & (1u << 5)) return KEY_BACK;
    if (newly & (1u << 4)) return KEY_OK;
    if (newly & (1u << 0)) return KEY_UP;
    if (newly & (1u << 1)) return KEY_DOWN;
    if (newly & (1u << 2)) return KEY_LEFT;
    if (newly & (1u << 3)) return KEY_RIGHT;
    return KEY_NONE;
}

// ---------------------------------------------------------------------------
// Menu rendering
// ---------------------------------------------------------------------------

void menu_draw(menu_ctx_t *ctx) {
    if (!menu_tft_active()) {
        if (!menu_tft_reinit()) return;
        ctx->tft_active = true;
    }

    int w = menu_tft_width();

    // Title bar (row 0)
    menu_str(0, 0, ctx->page->title, MENU_COLS, MC_TITLE, MC_TITBG, MENU_SCALE);

    // Separator (row 1)
    menu_rect(0, MENU_CELL_H, w, MENU_CELL_H, MC_SEP);

    // Menu items (rows 2..N)
    int vis = VISIBLE_ITEMS;
    int y = 2 * MENU_CELL_H;
    for (int i = 0; i < vis; i++) {
        int idx = ctx->scroll_top + i;
        if (idx < ctx->page->count) {
            bool sel = (idx == ctx->cursor);
            uint16_t fg = sel ? MC_HITEXT : MC_FG;
            uint16_t bg = sel ? MC_HILITE : MC_BG;

            int cols = MENU_COLS;
            char line[40];
            const menu_item_t *item = &ctx->page->items[idx];
            if (item->sub) {
                snprintf(line, sizeof(line), "%c %-*s>",
                         sel ? '>' : ' ', cols - 3, item->label);
            } else {
                snprintf(line, sizeof(line), "%c %-*s",
                         sel ? '>' : ' ', cols - 2, item->label);
            }
            menu_str(0, y, line, cols, fg, bg, MENU_SCALE);
        } else {
            menu_rect(0, y, w, MENU_CELL_H, MC_BG);
        }
        y += MENU_CELL_H;
    }

    // Separator
    int sep_row = 2 + vis;
    menu_rect(0, sep_row * MENU_CELL_H, w, MENU_CELL_H, MC_SEP);

    // Hint bar
    int hint_row = sep_row + 1;
    const char *hint = (ctx->parent)
        ? " [OK]Run [BACK]Back "
        : " [OK]Open [UP/DN]Nav";
    menu_str(0, hint_row * MENU_CELL_H, hint, MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
}

void menu_show_serial_banner(menu_ctx_t *ctx) {
    if (!menu_tft_active()) return;
    menu_clear(MC_BG);
    menu_str(0, 0, " Serial Active      ", MENU_COLS, MC_TITLE, MC_TITBG, MENU_SCALE);
    menu_str(0, 4 * MENU_CELL_H, "  COM port control  ", MENU_COLS, MC_FG, MC_BG, MENU_SCALE);
    menu_str(0, 6 * MENU_CELL_H, "  Menu resumes when ", MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
    menu_str(0, 7 * MENU_CELL_H, "  serial is idle.   ", MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
}

void menu_show_test_header(menu_ctx_t *ctx, const char *label) {
    if (!menu_tft_active()) return;
    menu_clear(MC_BG);
    menu_str(0, 0, " Running Test       ", MENU_COLS, MC_TITLE, MC_TITBG, MENU_SCALE);
    char line[21];
    snprintf(line, sizeof(line), "  %-18s", label);
    menu_str(0, 3 * MENU_CELL_H, line, MENU_COLS, MC_HITEXT, MC_BG, MENU_SCALE);
    menu_str(0, 6 * MENU_CELL_H, "  Output on serial. ", MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
    menu_str(0, 7 * MENU_CELL_H, "  BACK key to stop. ", MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
}

// ---------------------------------------------------------------------------
// Menu input handling
// ---------------------------------------------------------------------------

void menu_handle_key(menu_ctx_t *ctx, menu_key_t key) {
    switch (key) {
    case KEY_UP:
        if (ctx->cursor > 0) {
            ctx->cursor--;
        } else {
            // Wrap to last item
            ctx->cursor = ctx->page->count - 1;
        }
        // Adjust scroll window
        if (ctx->cursor < ctx->scroll_top)
            ctx->scroll_top = ctx->cursor;
        if (ctx->cursor >= ctx->scroll_top + VISIBLE_ITEMS)
            ctx->scroll_top = ctx->cursor - VISIBLE_ITEMS + 1;
        ctx->lcd_dirty = true;
        break;

    case KEY_DOWN:
        if (ctx->cursor < ctx->page->count - 1) {
            ctx->cursor++;
        } else {
            // Wrap to first item
            ctx->cursor = 0;
            ctx->scroll_top = 0;
        }
        // Adjust scroll window
        if (ctx->cursor >= ctx->scroll_top + VISIBLE_ITEMS)
            ctx->scroll_top = ctx->cursor - VISIBLE_ITEMS + 1;
        if (ctx->cursor < ctx->scroll_top)
            ctx->scroll_top = ctx->cursor;
        ctx->lcd_dirty = true;
        break;

    case KEY_OK: {
        const menu_item_t *item = &ctx->page->items[ctx->cursor];
        if (item->sub) {
            // Enter sub-page
            ctx->parent = ctx->page;
            ctx->page = item->sub;
            ctx->cursor = 0;
            ctx->scroll_top = 0;
            ctx->lcd_dirty = true;
        } else if (item->run) {
            // Run test
            ctx->state = MS_TEST_RUNNING;
            menu_show_test_header(ctx, item->label);

            // Init BACK key for test interruption
            back_key_init();

            printf("\n--- [MENU] Running: %s ---\n", item->label);
            item->run();
            printf("--- [MENU] Done: %s ---\n", item->label);

            back_key_deinit();
            key_gpio_init();   // Restore Row 0 GPIO as OUTPUT after back_key_deinit changed it to INPUT

            // Return to menu — force TFT reinit because tests (tft, tft_fast,
            // gnss_tft) may have released PIO GPIO or changed backlight state.
            menu_tft_stop();
            ctx->state = MS_MENU;
            ctx->tft_active = false;
            ctx->lcd_dirty = true;

            // Brief pause to avoid spurious key re-trigger
            sleep_ms(200);
        }
        break;
    }

    case KEY_BACK:
    case KEY_LEFT:
        if (ctx->parent) {
            // Find which root item pointed to the current page, to restore cursor
            const menu_page_t *p = ctx->parent;
            uint8_t prev_cursor = 0;
            for (uint8_t i = 0; i < p->count; i++) {
                if (p->items[i].sub == ctx->page) { prev_cursor = i; break; }
            }
            ctx->page = p;
            ctx->parent = (p == &root_page) ? NULL : NULL;  // only 2 levels
            ctx->cursor = prev_cursor;
            ctx->scroll_top = 0;
            ctx->lcd_dirty = true;
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Menu initialization
// ---------------------------------------------------------------------------

void menu_init(menu_ctx_t *ctx) {
    // Init keyboard GPIO (stays active for the entire session)
    key_gpio_init();

    // Init TFT + backlight
    if (menu_tft_init()) {
        ctx->tft_active = true;
    } else {
        ctx->tft_active = false;
        printf("WARNING: TFT init failed; menu display unavailable\n");
    }

    // Set initial menu state
    ctx->state = MS_MENU;
    ctx->page = &root_page;
    ctx->parent = NULL;
    ctx->cursor = 0;
    ctx->scroll_top = 0;
    ctx->serial_last_ms = 0;
    ctx->lcd_dirty = true;
}
