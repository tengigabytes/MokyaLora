/* core1_bringup_test_main.c — Step 16 Stage B prerequisite.
 *
 * Validates that Core 1 can run the bringup REPL (all peripherals + USB CDC).
 *
 * Split responsibilities:
 *   Core 0: calls bringup_repl_init() — registers the USB CDC interrupt on
 *           Core 0's NVIC (required; multicore_launch_core1 skips per-core
 *           SDK runtime init so Core 1 cannot register IRQs directly).
 *           Then launches Core 1 and spins idle.
 *   Core 1: calls bringup_repl_loop() — runs the interactive REPL.  USB CDC
 *           is already live because Core 0 initialised it above.
 *
 * If this test passes it confirms Core 1 peripheral access works correctly
 * before FreeRTOS integration begins.
 */
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "bringup.h"

static void core1_entry(void) {
    /* USB CDC already initialised by Core 0 — run the REPL loop only. */
    bringup_repl_loop();
}

int main(void) {
    /* Core 0: init USB CDC (IRQ must be registered here), then hand off.
     *
     * multicore_reset_core1() is called before multicore_launch_core1().
     * This is required by the pico-sdk API: Core 1 must be in its boot-ROM
     * multicore-launch handler before the handshake sequence will succeed.
     * J-Link only resets Core 0 when flashing; Core 1 may still be running
     * previous user code.  multicore_reset_core1() uses the PSM to cycle
     * Core 1's power domain, which puts it back into the boot-ROM handler.
     */
    bringup_repl_init();
    multicore_reset_core1();   /* ensure Core 1 is in boot-ROM handler */
    multicore_launch_core1(core1_entry);
    while (true) {
        tight_loop_contents();
    }
}
