/* MokyaLora Phase 2 M1.1-B — Core 1 USB↔SPSC bridge (Pico SDK)
 *
 * Successor to the M1.1-A bare-metal ring validator. This image boots as a
 * full Pico SDK application: SDK crt0 + runtime_init (with dangerous hooks
 * skipped, see CMakeLists.txt PICO_RUNTIME_SKIP_INIT_* defines) + FreeRTOS
 * RP2350_ARM_NTZ port + TinyUSB device stack.
 *
 * Vector table:
 *   Provided by pico-sdk/src/rp2_common/pico_crt0/crt0.S — 48 slots, first
 *   word = __StackTop, second word = _reset_handler, all ISR slots weakly
 *   aliased to isr_invalid. TinyUSB's dcd_rp2040 installs the USBCTRL IRQ
 *   handler at runtime via irq_set_exclusive_handler(). FreeRTOS port
 *   installs SVC/PendSV/SysTick via exception_set_exclusive_handler() at
 *   scheduler start, writing into the SDK's ram_vector_table (populated by
 *   runtime_init_install_ram_vector_table on boot).
 *
 * Boot handshake (same as M1.1-A):
 *   1. Core 0 calls ipc_shared_init() which zeroes g_ipc_shared and stores
 *      IPC_BOOT_MAGIC in boot_magic as the "ready" doorbell.
 *   2. Core 0 calls multicore_launch_core1_raw(), which reads SP/PC from
 *      flash 0x10200000 and wakes Core 1.
 *   3. Core 1 SDK crt0 runs runtime_init hooks, calls main().
 *   4. main() stamps the M1.1-B sentinel (0xC1B01200) at 0x20078000 and
 *      spins on g_ipc_shared.boot_magic until it sees IPC_BOOT_MAGIC. This
 *      is belt-and-braces: Core 0 sets boot_magic BEFORE launching us, so
 *      the first read should already see it.
 *   5. main() resets USBCTRL, calls tusb_init(), and busy-polls tud_task()
 *      for ~2 s so the host PC enumerates the CDC device.
 *   6. main() publishes c1_ready = 1 and pushes a greeting log line into
 *      c1_to_c0.
 *   7. main() creates usb_device_task + bridge_task and starts the
 *      FreeRTOS scheduler. It never returns.
 *
 * Operational counters (SWD-readable via `mem32`, survive resets as long as
 * RAM is not cleared). Every new slot must be registered in
 * firmware-architecture.md §9.3 before use; see feedback on breadcrumb
 * lifecycle discipline — solved-issue breadcrumbs are released in the fix
 * commit, not left as permanent fixtures:
 *   0x2007FFC4  rx total   — bytes drained from c0_to_c1 ring (→ CDC IN)
 *   0x2007FFC8  tx total   — bytes drained from CDC OUT (→ c1_to_c0 ring)
 *   0x2007FFCC  usb state  — bit0=mounted, bit1=cdc_connected (DTR)
 *
 * Note on tud_cdc_connected() vs tud_mounted():
 *   tud_cdc_connected() returns true only after the host sends
 *   SET_CONTROL_LINE_STATE with the DTR bit set. pyserial (and hence the
 *   Meshtastic CLI) does this by default, but Chrome WebSerial, some Linux
 *   /dev/ttyACM consumers, and Meshtastic's web console do not. We therefore
 *   gate CDC writes on tud_mounted() instead (USB enumerated) and rely on a
 *   bounded FIFO-full retry to drop bursts when no client is actually reading
 *   — this way the bit-0 mounted bit reflects "can forward data" and bit-1 is
 *   just diagnostic.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/regs/resets.h"
#include "hardware/sync.h"
#include "hardware/structs/sio.h"
#include "tusb.h"

#include "FreeRTOS.h"
#include "task.h"

#include "ipc_protocol.h"
#include "ipc_shared_layout.h"
#include "ipc_ringbuf.h"

#include "i2c_bus.h"
#include "bq25622.h"
#include "sensor_task.h"
#include "display.h"
#include "lvgl_glue.h"
#include "keypad_scan.h"
#include "key_event.h"

/* ── Doorbell IPC notification (M2) ─────────────────────────────────────── *
 * We use raw SIO register writes instead of pico_multicore API to avoid
 * runtime_init side effects from linking pico_multicore into Core 1.       */
#define NOTIFY_BIT_IPC  (1u << 0)
#ifndef SIO_IRQ_BELL
#define SIO_IRQ_BELL  26   /* RP2350 doorbell IRQ (secure) */
#endif

static TaskHandle_t g_bridge_task_handle;

static inline void doorbell_set_other_core(uint32_t num)
{
    sio_hw->doorbell_out_set = 1u << num;
}

static inline void doorbell_clear_own(uint32_t num)
{
    sio_hw->doorbell_in_clr = 1u << num;
}

/* ── Operational counters in .shared_ipc tail pad ──────────────────────── */
/* Last 64 B of .shared_ipc — guaranteed outside Core 0's heap & task stacks.
 * Registered slots only (see firmware-architecture.md §9.3).                */
#define BRIDGE_RX_COUNT_ADDR   0x2007FFC4u
#define BRIDGE_TX_COUNT_ADDR   0x2007FFC8u
#define BRIDGE_USB_STATE_ADDR  0x2007FFCCu

#define USB_STATE_MOUNTED      (1u << 0)
#define USB_STATE_CDC          (1u << 1)

static inline void dbg_u32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static volatile uint32_t g_rx_total;
static volatile uint32_t g_tx_total;

/* ── Flash-park handler (MUST reside in RAM — runs while XIP is off) ───── *
 *
 * Called from the doorbell ISR when Core 0 requests a flash write.
 * Protocol:
 *   1. Core 0 sets flash_lock = REQUEST, fires IPC_FLASH_DOORBELL
 *   2. This ISR catches the doorbell, ACKs with flash_lock = PARKED, __SEV()
 *   3. Disables all interrupts on Core 1 (SysTick, USB, etc.)
 *   4. Spins in RAM until Core 0 clears flash_lock back to IDLE + __SEV()
 *   5. Re-enables interrupts and returns — scheduler resumes
 *
 * The __no_inline_not_in_flash_func attribute guarantees the function body
 * and all local data are placed in .time_critical (SRAM), not .text (flash).
 */
static void __no_inline_not_in_flash_func(flash_park_handler)(void)
{
    /* ACK: tell Core 0 we are safely parked */
    __atomic_store_n(&g_ipc_shared.flash_lock,
                     IPC_FLASH_LOCK_PARKED, __ATOMIC_RELEASE);
    __sev();  /* wake Core 0 polling on flash_lock */

    /* Kill ALL interrupts on this core — no SysTick, no USB, nothing
     * can trigger a flash fetch while XIP is off. */
    uint32_t saved = save_and_disable_interrupts();

    /* Spin in RAM until Core 0 clears the lock. __wfe() is a low-power
     * wait; Core 0 will __sev() after the flash operation completes. */
    while (__atomic_load_n(&g_ipc_shared.flash_lock,
                           __ATOMIC_ACQUIRE) != IPC_FLASH_LOCK_IDLE) {
        __wfe();
    }

    restore_interrupts(saved);
}

/* ── Doorbell ISR — Core 0 ring push wakes bridge_task ──────────────────── */

static void __no_inline_not_in_flash_func(ipc_doorbell_isr)(void)
{
    /* Check flash-park doorbell FIRST — time-critical */
    if (sio_hw->doorbell_in_set & (1u << IPC_FLASH_DOORBELL)) {
        doorbell_clear_own(IPC_FLASH_DOORBELL);
        if (__atomic_load_n(&g_ipc_shared.flash_lock,
                            __ATOMIC_ACQUIRE) == IPC_FLASH_LOCK_REQUEST) {
            flash_park_handler();
        }
    }

    /* Normal IPC ring doorbell */
    if (sio_hw->doorbell_in_set & (1u << IPC_DOORBELL_NUM)) {
        doorbell_clear_own(IPC_DOORBELL_NUM);
        if (g_bridge_task_handle != NULL) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xTaskNotifyFromISR(g_bridge_task_handle, NOTIFY_BIT_IPC,
                               eSetBits, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }
}

/* ── FreeRTOS tasks ──────────────────────────────────────────────────────── */

/* TinyUSB polling task. OPT_OS_NONE means tud_task() is a non-blocking
 * poll — we call it as fast as practical while still yielding to the
 * bridge task via taskYIELD(). */
static void usb_device_task(void *pv)
{
    (void)pv;
    for (;;) {
        tud_task();
        /* Cooperative yield — no 1 ms floor, so the bridge task runs
         * immediately after each TinyUSB poll. */
        taskYIELD();
    }
}

/* Byte bridge task.
 *   c0_to_c1 ring  ->  tud_cdc_write_n  (host sees Meshtastic output)
 *   tud_cdc_read_n ->  c1_to_c0 ring    (host input reaches Meshtastic)
 *
 * Both directions use IPC_MSG_SERIAL_BYTES slots. Ring slots carry up to
 * IPC_MSG_PAYLOAD_MAX (256 B) at a time; we pass the whole slot straight to
 * TinyUSB. If the TinyUSB TX fifo is full we wait a tick and retry — the
 * c0_to_c1 ring is 32 × 264 B = 8 KB, which is plenty to buffer any burst
 * while the host drains its end.
 */
static void bridge_task(void *pv)
{
    (void)pv;

    uint8_t scratch[IPC_MSG_PAYLOAD_MAX];
    uint8_t tx_seq = 0;
    bool reboot_pending = false;

    /* Enable doorbell IRQ now that the FreeRTOS scheduler is running and
     * PendSV/SVC handlers are installed.  Any pending doorbells from
     * Core 0 during the pre-scheduler busy-wait will fire immediately. */
    irq_set_enabled(SIO_IRQ_BELL, true);

    for (;;) {
        /* If Core 0 announced a reboot, stop all ring/CDC processing and
         * idle until the watchdog fires the chip-wide reset. */
        if (reboot_pending) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool did_work = false;

        /* ── c0_to_c1 DATA ring → CDC IN (high priority) ───────────── */
        IpcMsgHeader hdr;
        if (ipc_ring_pop(&g_ipc_shared.c0_to_c1_ctrl,
                         g_ipc_shared.c0_to_c1_slots,
                         IPC_RING_SLOT_COUNT,
                         &hdr,
                         scratch,
                         sizeof(scratch))) {

            /* ── Message dispatch ─────────────────────────────────────── */
            if (hdr.msg_id == IPC_MSG_REBOOT_NOTIFY) {
                /* Core 0 is about to watchdog-reset the chip.  Gracefully
                 * disconnect USB so the host drops the COM port handle
                 * before the hard reset yanks the controller out. */
                tud_disconnect();
                reboot_pending = true;
                did_work = true;
                continue;   /* skip the rest of this iteration */
            }

            if (hdr.msg_id == IPC_MSG_SERIAL_BYTES && hdr.payload_len > 0u) {
                uint16_t remaining = hdr.payload_len;
                const uint8_t *p = scratch;
                /* Bounded FIFO-full retries: if the host isn't actively
                 * draining, give up after ~10 ms and drop the burst so the
                 * c0_to_c1 ring keeps moving. Meshtastic's serial protocol
                 * retransmits on demand, so burst loss while no client is
                 * attached is benign. */
                int stall_ticks = 0;
                while (remaining > 0u) {
                    if (!tud_mounted()) {
                        break;
                    }
                    uint32_t avail = tud_cdc_write_available();
                    if (avail == 0u) {
                        tud_cdc_write_flush();
                        if (++stall_ticks >= 10) {
                            break;
                        }
                        taskYIELD();  /* was vTaskDelay(1ms) — yield to usb_device_task for tud_task() processing, ~10µs vs 1ms */
                        continue;
                    }
                    stall_ticks = 0;
                    uint32_t chunk = (remaining < avail) ? remaining : avail;
                    uint32_t wrote = tud_cdc_write(p, chunk);
                    p += wrote;
                    remaining -= (uint16_t)wrote;
                    g_rx_total += wrote;
                }
                tud_cdc_write_flush();
                dbg_u32(BRIDGE_RX_COUNT_ADDR, g_rx_total);
            }
            did_work = true;
        }

        /* ── c0_log_to_c1 LOG ring → CDC IN (low priority) ──────────
         * Drain log slots only when the data ring is empty, so protobuf
         * frames are never delayed by log forwarding. Log output is
         * best-effort — if CDC FIFO is full, drop immediately. */
        else if (ipc_ring_pop(&g_ipc_shared.c0_log_to_c1_ctrl,
                               g_ipc_shared.c0_log_to_c1_slots,
                               IPC_LOG_RING_SLOT_COUNT,
                               &hdr,
                               scratch,
                               sizeof(scratch))) {
            if (hdr.msg_id == IPC_MSG_SERIAL_BYTES && hdr.payload_len > 0u
                && tud_mounted()) {
                uint32_t avail = tud_cdc_write_available();
                uint32_t to_write = (hdr.payload_len < avail)
                                        ? hdr.payload_len : avail;
                if (to_write > 0u) {
                    uint32_t wrote = tud_cdc_write(scratch, to_write);
                    g_rx_total += wrote;
                    tud_cdc_write_flush();
                    dbg_u32(BRIDGE_RX_COUNT_ADDR, g_rx_total);
                }
            }
            did_work = true;
        }

        /* ── CDC OUT → c1_to_c0 CMD ring ──────────────────────────────── */
        if (tud_cdc_available()) {
            uint32_t n = tud_cdc_read(scratch, sizeof(scratch));
            if (n > 0u) {
                /* Push in one slot — if the ring is full, we spin until
                 * Core 0 drains. Typing rate on a human terminal will
                 * never fill a 32-slot ring. */
                while (!ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                                      g_ipc_shared.c1_to_c0_slots,
                                      IPC_RING_SLOT_COUNT,
                                      IPC_MSG_SERIAL_BYTES,
                                      tx_seq,
                                      scratch,
                                      (uint16_t)n)) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                tx_seq++;
                g_tx_total += n;
                dbg_u32(BRIDGE_TX_COUNT_ADDR, g_tx_total);
                /* Wake Core 0 — fire-and-forget (Core 0 has no doorbell ISR,
                 * its FreeRTOS pico_sync handler is patched out). */
                doorbell_set_other_core(IPC_DOORBELL_NUM);
                did_work = true;
            }
        }

        /* Refresh USB state counter (<= 100 Hz due to 10 ms notify wait). */
        {
            uint32_t st = 0;
            if (tud_mounted())       st |= USB_STATE_MOUNTED;
            if (tud_cdc_connected()) st |= USB_STATE_CDC;
            dbg_u32(BRIDGE_USB_STATE_ADDR, st);
        }

        if (!did_work) {
            /* No data in either direction — block until doorbell fires
             * (Core 0 pushed new ring data) or 10 ms timeout (handles
             * CDC OUT events not signaled by doorbell). */
            xTaskNotifyWait(0, UINT32_MAX, NULL, pdMS_TO_TICKS(10));
        }
    }
}

/* ── FreeRTOS SysTick override ──────────────────────────────────────────────
 * The RP2350_ARM_NTZ port's default vPortSetupTimerInterrupt() computes the
 * SysTick reload from clock_get_hz(clk_sys). But Core 1 skips
 * runtime_init_clocks (Core 0 owns clock init), so configured_freq[clk_sys]
 * stays 0 and the reload silently wraps to 0x00FFFFFF — tick rate collapses
 * to ~9 Hz, making vTaskDelay(1000 ms) wait ~112 s. Override with the known
 * configCPU_CLOCK_HZ (Arduino-Pico default 150 MHz) so SysTick fires at the
 * configured configTICK_RATE_HZ. */
#define MOKYA_SYSTICK_CTRL    ( *( ( volatile uint32_t * ) 0xE000E010 ) )
#define MOKYA_SYSTICK_LOAD    ( *( ( volatile uint32_t * ) 0xE000E014 ) )
#define MOKYA_SYSTICK_CURRENT ( *( ( volatile uint32_t * ) 0xE000E018 ) )

void vPortSetupTimerInterrupt(void)
{
    MOKYA_SYSTICK_CTRL    = 0x00000004u;  /* CLKSOURCE=proc, disabled */
    MOKYA_SYSTICK_CURRENT = 0u;
    MOKYA_SYSTICK_LOAD    = (configCPU_CLOCK_HZ / configTICK_RATE_HZ) - 1u;
    MOKYA_SYSTICK_CTRL    = 0x00000007u;  /* CLKSOURCE=proc | TICKINT | ENABLE */
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    /* Zero operational counters in the tail pad so SWD sees 0 before the
     * bridge task starts publishing real values. */
    dbg_u32(BRIDGE_RX_COUNT_ADDR,   0u);
    dbg_u32(BRIDGE_TX_COUNT_ADDR,   0u);
    dbg_u32(BRIDGE_USB_STATE_ADDR,  0u);
    __dmb();

    /* 2. Wait for Core 0's shared-SRAM publication. Core 0 writes
     * IPC_BOOT_MAGIC before it launches us, so this should observe the
     * ready value on the very first load — but we spin defensively in
     * case of a cold-boot race. */
    while (__atomic_load_n(&g_ipc_shared.boot_magic, __ATOMIC_ACQUIRE)
           != IPC_BOOT_MAGIC) {
        __asm volatile ("yield");
    }

    /* 3. Force USB re-enumeration.
     *
     * Core 0 / SDK runtime may have left USBCTRL in a half-initialised state
     * — e.g. after J-Link SWD reset Windows never sees a disconnect event
     * because D+ pullup stayed high. Resetting the USBCTRL block drops D+,
     * holding for 20 ms guarantees the host detects disconnect, and then
     * unreset_block_wait releases it so tusb_init() can do a clean start.
     * This is the same pattern proven in firmware/tools/bringup/
     * core1_rtos_efgh.c (Step 16 Stage B2). */
    reset_block(RESETS_RESET_USBCTRL_BITS);
    busy_wait_ms(20);
    unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

    /* 4. Bring TinyUSB up (registers USBCTRL_IRQ via irq_set_exclusive_handler
     * inside dcd_rp2040_init). */
    tusb_init();

    /* 5. Busy-poll tud_task() long enough for the host to enumerate the CDC
     * device before we hand off to FreeRTOS. 2000 iterations × 1 ms =~ 2 s.
     * This is what Stage B2 does — it's not strictly required (the USB
     * device task will keep polling after scheduler starts) but it means
     * the COM port shows up before Meshtastic starts logging, so we don't
     * lose the boot banner. */
    for (int i = 0; i < 2000; i++) {
        tud_task();
        busy_wait_ms(1);
    }

    /* 6. Announce ourselves on the c1_to_c0 ring. Matches M1.1-A greeting. */
    {
        const IpcPayloadLogLine hdr = {
            .level    = 1u, /* INFO */
            .core     = 1u,
            .text_len = 16u,
        };
        uint8_t packet[sizeof(hdr) + 16u];
        memcpy(packet, &hdr, sizeof(hdr));
        memcpy(packet + sizeof(hdr), "core1 bridge up", 16u);
        (void)ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                            g_ipc_shared.c1_to_c0_slots,
                            IPC_RING_SLOT_COUNT,
                            IPC_MSG_LOG_LINE,
                            0u,
                            packet,
                            (uint16_t)sizeof(packet));
    }

    __atomic_store_n(&g_ipc_shared.c1_ready, 1u, __ATOMIC_RELEASE);

    /* Bring up both I2C buses before any driver starts. display_init()'s
     * backlight setup and future power/sensor drivers all depend on this. */
    i2c_bus_init_all();

    /* 7. Register doorbell ISR but do NOT enable yet — enabling before the
     *    FreeRTOS scheduler starts causes a HardFault because portYIELD_FROM_ISR
     *    triggers PendSV before the port has installed the PendSV handler.
     *    The IRQ is enabled at the start of bridge_task (after scheduler runs). */
    irq_set_exclusive_handler(SIO_IRQ_BELL, ipc_doorbell_isr);

    /* 8. Hand off to FreeRTOS. */
    /* Both tasks at the same priority so taskYIELD() round-robins
     * between them without the 1 ms vTaskDelay floor. */
    BaseType_t rc_usb = xTaskCreate(usb_device_task, "usb",    1024, NULL,
                tskIDLE_PRIORITY + 2, NULL);
    BaseType_t rc_brg = xTaskCreate(bridge_task,     "bridge", 1024, NULL,
                tskIDLE_PRIORITY + 2, &g_bridge_task_handle);
    /* LVGL service task runs at the same priority as usb / bridge so the
     * round-robin scheduler hands it CPU between their yields. A lower
     * priority would be starved by usb_device_task's tight tud_task()
     * + taskYIELD() loop. lvgl_task owns display_init() — main() must not
     * also call it. */
    BaseType_t rc_dsp = lvgl_glue_start(tskIDLE_PRIORITY + 2);

    /* KeyEvent queue — allocate before creating keypad_scan_task so the
     * very first debounce commit (possible within ~20 ms of boot if a
     * key is held) finds the queue ready. Producer/consumer are both
     * future tasks; only the scan task enqueues in Phase B. */
    key_event_init();

    /* Keypad scan task — MUST run at the same priority as usb / bridge / lvgl.
     * usb_device_task is a `tud_task(); taskYIELD();` loop with no blocking
     * call, so it is always Ready at priority tskIDLE_PRIORITY + 2. Under
     * preemptive scheduling, any strictly-lower-priority task is starved.
     * Equal priority enables round-robin time-slicing, giving keypad its
     * slot between the 5 ms debounce pauses. 512-word stack is plenty
     * for the loop with 6-byte local raw buffer and no nested calls. */
    BaseType_t rc_kp = xTaskCreate(keypad_scan_task, "kpad", 512, NULL,
                tskIDLE_PRIORITY + 2, NULL);

    /* Charger (BQ25622) 1 Hz poll task — hw init runs inside the task so
     * first I2C traffic is post-scheduler, after i2c_bus mutex is usable. */
    bool rc_chg = bq25622_start_task(tskIDLE_PRIORITY + 2);

    /* Sensor bus poll task (M3.4.5). Init + 1 Hz baro poll today; LIS2MDL
     * and LSM6DSV16X slot into the same tick in M3.4.5b / .5c. */
    bool rc_sns = sensor_task_start(tskIDLE_PRIORITY + 2);

    (void)rc_usb; (void)rc_brg; (void)rc_dsp; (void)rc_kp; (void)rc_chg;
    (void)rc_sns;

    vTaskStartScheduler();

    /* Should never reach here. */
    for (;;) {
        __asm volatile ("wfi");
    }
}
