/* main_core0.c — thin main() wrapper for the i2c_custom_scan (Core 0) binary.
 * Exists so that i2c_custom_scan.c can export bringup_repl_run() without
 * defining its own main(). */
#include "bringup.h"

int main(void) {
    bringup_repl_run();
}
