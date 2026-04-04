#include "bringup.h"
#include "pico/multicore.h"
#include "hardware/sync.h"

// ---------------------------------------------------------------------------
// Inter-core FIFO protocol tokens
// ---------------------------------------------------------------------------
#define C1_READY         0xC1B00700u   // Core 1 → Core 0: booted and ready
#define C0_CMD_ECHO      0x00000001u   // Core 0: run 4-value echo round-trip
#define C0_CMD_SRAM      0x00000002u   // Core 0: read shared SRAM[0] and return
#define C0_CMD_GPIO      0x00000003u   // Core 0: toggle GPIO from Core 1
#define C1_GPIO_ACK      0xC1670003u   // Core 1 → Core 0: GPIO toggle done
#define C0_CMD_SHUTDOWN  0xDEADDEADu   // Core 0: shut down Core 1 loop

// ---------------------------------------------------------------------------
// Shared state (both cores access these)
// ---------------------------------------------------------------------------
static volatile uint32_t c1_shared[4];
static uint c1_spinlock_num;

// ---------------------------------------------------------------------------
// Core 1 entry — bare-metal, no FreeRTOS (Stage A)
// ---------------------------------------------------------------------------
static void core1_entry(void) {
    // Initialise motor GPIO as plain output — safe for brief toggles,
    // proves Core 1 can access GPIO peripheral registers.
    gpio_init(MTR_PWM_PIN);
    gpio_set_dir(MTR_PWM_PIN, GPIO_OUT);
    gpio_put(MTR_PWM_PIN, 0);

    // Notify Core 0 that Core 1 is alive and ready for commands.
    multicore_fifo_push_blocking(C1_READY);

    while (true) {
        uint32_t cmd = multicore_fifo_pop_blocking();

        if (cmd == C0_CMD_SHUTDOWN) break;

        if (cmd == C0_CMD_ECHO) {
            // Read 4 probe values and echo each back unchanged.
            for (int i = 0; i < 4; i++) {
                uint32_t val = multicore_fifo_pop_blocking();
                multicore_fifo_push_blocking(val);
            }
        } else if (cmd == C0_CMD_SRAM) {
            // Read shared SRAM[0] under spinlock and push the value to Core 0.
            spin_lock_t *lock = spin_lock_instance(c1_spinlock_num);
            uint32_t save = spin_lock_blocking(lock);
            uint32_t v = c1_shared[0];
            spin_unlock(lock, save);
            multicore_fifo_push_blocking(v);
        } else if (cmd == C0_CMD_GPIO) {
            // Toggle motor pin twice (~60 ms) to confirm peripheral access.
            gpio_put(MTR_PWM_PIN, 1); sleep_ms(20);
            gpio_put(MTR_PWM_PIN, 0); sleep_ms(20);
            gpio_put(MTR_PWM_PIN, 1); sleep_ms(20);
            gpio_put(MTR_PWM_PIN, 0);
            multicore_fifo_push_blocking(C1_GPIO_ACK);
        }
    }

    // Restore pin to safe state before Core 1 halts.
    gpio_put(MTR_PWM_PIN, 0);
    gpio_set_dir(MTR_PWM_PIN, GPIO_IN);
}

// ---------------------------------------------------------------------------
// core1_test — Step 16 Stage A, called from Core 0 command dispatch
// ---------------------------------------------------------------------------
void core1_test(void) {
    printf("\n--- Step 16 Stage A: Core 1 bare-metal ---\n");

    // Claim a hardware spinlock for the SRAM test; panic if none are free.
    c1_spinlock_num = spin_lock_claim_unused(true);
    spin_lock_t *lock = spin_lock_instance(c1_spinlock_num);

    // Discard any stale values left in the FIFO from a previous run.
    multicore_fifo_drain();

    // -----------------------------------------------------------------------
    // [1/4] Core 1 boot
    // -----------------------------------------------------------------------
    printf("[1/4] Core 1 boot... ");
    multicore_launch_core1(core1_entry);

    uint32_t ready = 0;
    if (!multicore_fifo_pop_timeout_us(2000000, &ready)) {
        printf("FAIL (timeout — Core 1 did not respond)\n");
        goto cleanup;
    }
    if (ready != C1_READY) {
        printf("FAIL (got 0x%08X, expected 0x%08X)\n", ready, C1_READY);
        goto cleanup;
    }
    printf("PASS (0x%08X)\n", ready);

    // -----------------------------------------------------------------------
    // [2/4] Inter-core FIFO echo (4 round-trips)
    // -----------------------------------------------------------------------
    printf("[2/4] FIFO echo (4 round-trips)... ");
    {
        static const uint32_t probes[4] = {
            0x11223344u, 0xAABBCCDDu, 0xDEADBEEFu, 0x12345678u
        };
        multicore_fifo_push_blocking(C0_CMD_ECHO);
        bool ok = true;
        for (int i = 0; i < 4; i++) {
            multicore_fifo_push_blocking(probes[i]);
            uint32_t echo = 0;
            if (!multicore_fifo_pop_timeout_us(500000, &echo)) {
                printf("FAIL (probe[%d] timeout)\n", i);
                ok = false;
                break;
            }
            if (echo != probes[i]) {
                printf("FAIL (probe[%d]: sent 0x%08X got 0x%08X)\n",
                       i, probes[i], echo);
                ok = false;
                break;
            }
        }
        if (ok) printf("PASS (4/4)\n");
    }

    // -----------------------------------------------------------------------
    // [3/4] Shared SRAM + spinlock
    // -----------------------------------------------------------------------
    printf("[3/4] Shared SRAM (spinlock)... ");
    {
        const uint32_t test_val = 0xBEEFC0DEu;
        uint32_t save = spin_lock_blocking(lock);
        c1_shared[0] = test_val;
        spin_unlock(lock, save);

        multicore_fifo_push_blocking(C0_CMD_SRAM);
        uint32_t c1_read = 0;
        if (!multicore_fifo_pop_timeout_us(500000, &c1_read)) {
            printf("FAIL (timeout)\n");
        } else if (c1_read == test_val) {
            printf("PASS (Core 1 read 0x%08X)\n", c1_read);
        } else {
            printf("FAIL (wrote 0x%08X, Core 1 read 0x%08X)\n", test_val, c1_read);
        }
    }

    // -----------------------------------------------------------------------
    // [4/4] Core 1 peripheral — GPIO toggle from Core 1
    // -----------------------------------------------------------------------
    printf("[4/4] Core 1 GPIO (motor pin GPIO %d)... ", MTR_PWM_PIN);
    {
        multicore_fifo_push_blocking(C0_CMD_GPIO);
        uint32_t gpio_ack = 0;
        if (!multicore_fifo_pop_timeout_us(1000000, &gpio_ack)) {
            printf("FAIL (timeout)\n");
        } else if (gpio_ack == C1_GPIO_ACK) {
            printf("PASS\n");
        } else {
            printf("FAIL (got 0x%08X)\n", gpio_ack);
        }
    }

cleanup:
    // Shut down Core 1 cleanly, then release resources.
    multicore_fifo_push_blocking(C0_CMD_SHUTDOWN);
    sleep_ms(100);
    multicore_reset_core1();
    spin_lock_unclaim(c1_spinlock_num);
    printf("--- Stage A complete ---\n");
}
