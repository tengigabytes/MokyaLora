#pragma once

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Navigation key codes (abstract events from the 6x6 keypad matrix)
// ---------------------------------------------------------------------------

typedef enum {
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_OK,
    KEY_BACK,
} menu_key_t;

// ---------------------------------------------------------------------------
// Menu data structures
// ---------------------------------------------------------------------------

typedef struct menu_page menu_page_t;

typedef struct {
    const char       *label;    // display name, max ~18 chars
    void            (*run)(void);   // non-NULL = leaf test function
    const menu_page_t *sub;     // non-NULL = sub-page (run must be NULL)
} menu_item_t;

struct menu_page {
    const char        *title;   // page title for header row
    const menu_item_t *items;   // array of items
    uint8_t            count;   // number of items
};

typedef enum {
    MS_MENU,            // browsing menu on LCD
    MS_TEST_RUNNING,    // a test function is executing
    MS_SERIAL_ACTIVE,   // serial host has taken over
} menu_state_t;

typedef struct {
    menu_state_t        state;
    const menu_page_t  *page;           // current page
    const menu_page_t  *parent;         // parent page (NULL = root)
    uint8_t             cursor;         // highlighted item index
    uint8_t             scroll_top;     // first visible item (for scrolling)
    uint32_t            serial_last_ms; // timestamp of last serial byte
    bool                lcd_dirty;      // needs redraw
    bool                tft_active;     // TFT PIO is currently claimed by menu
} menu_ctx_t;

// ---------------------------------------------------------------------------
// Shared TFT text rendering (extracted from bringup_gnss_tft.c)
// Uses PIO1, ST7789VI 240x320 RGB565, 5x8 bitmap font.
// ---------------------------------------------------------------------------

// 5x8 bitmap font — column-major, LSB = top row, chars 0x20-0x7E
extern const uint8_t MENU_FONT5[95][5];

// PIO lifecycle — claim/release PIO1 SM for 8080 writes
bool menu_tft_start(void);     // claim SM + load program; returns false on error
void menu_tft_stop(void);      // release SM + program
bool menu_tft_active(void);    // true if PIO SM is currently claimed

// ST7789VI full init (SWRESET, SLPOUT, COLMOD, gamma, INVON, DISPON)
void menu_tft_hw_init(void);

// Full TFT init: GPIO + PIO + ST7789 + backlight ON
bool menu_tft_init(void);

// Re-init TFT after a test that claimed/released PIO1
bool menu_tft_reinit(void);

// Cycle screen rotation: 0° → 90° → 180° → 270° → 0°
// Updates MADCTL and internal width/height. Returns new rotation in degrees.
int menu_tft_rotate(void);

// Get current screen dimensions (accounts for rotation)
int menu_tft_width(void);
int menu_tft_height(void);

// Low-level drawing primitives
void menu_tft_flush(void);
void menu_tft_cmd(uint8_t c);
void menu_tft_dat(uint8_t d);
void menu_tft_w8(uint8_t b);
void menu_tft_win(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void menu_rect(int x, int y, int w, int h, uint16_t color);
void menu_clear(uint16_t color);

// Text rendering (scale=1: 6x8 px/char, scale=2: 12x16 px/char)
void menu_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
void menu_str(int x, int y, const char *s, int width,
              uint16_t fg, uint16_t bg, int scale);

// ---------------------------------------------------------------------------
// Menu system API
// ---------------------------------------------------------------------------

// Get the root menu page
const menu_page_t *menu_root_page(void);

// Initialize menu context + TFT + backlight + keyboard
void menu_init(menu_ctx_t *ctx);

// Draw the current menu page on the TFT
void menu_draw(menu_ctx_t *ctx);

// Show "Serial Active" banner on LCD
void menu_show_serial_banner(menu_ctx_t *ctx);

// Show test header before running a test
void menu_show_test_header(menu_ctx_t *ctx, const char *label);

// Scan navigation keys (non-blocking, debounced, edge-detect)
menu_key_t menu_scan_key(void);

// Handle a navigation key press (updates ctx, sets lcd_dirty)
void menu_handle_key(menu_ctx_t *ctx, menu_key_t key);

// RGB565 colour constants shared with gnss_tft
#define MC_BG     0x0000   // black
#define MC_FG     0xC618   // light gray
#define MC_HILITE 0x001F   // blue (highlight bar)
#define MC_HITEXT 0xFFFF   // white (highlighted text)
#define MC_TITLE  0xFFE0   // yellow (title text)
#define MC_TITBG  0x0010   // dark blue (title bar)
#define MC_HINT   0x7BEF   // medium gray (hint bar)
#define MC_SEP    0x4208   // dark gray (separator)
#define MC_OK     0x07E0   // green  (pass indicator)
#define MC_ERR    0xF800   // red    (fail indicator)
