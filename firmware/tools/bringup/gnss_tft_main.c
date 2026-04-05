/* gnss_tft_main.c — standalone entry point for Step 14 GPS field test.
 *
 * Boots directly into gnss_tft_test() without the bringup REPL.
 * Intended for use without a USB cable; TFT displays live GPS data.
 * Press the BACK key (R0C1) on the physical keypad to exit. */

#include "bringup.h"

int main(void) {
    stdio_init_all();   // no-op when USB is absent; enables printf when plugged in
    gnss_tft_test();
    // After exit (BACK key), freeze — do not restart TFT or REPL
    while (true) tight_loop_contents();
}
