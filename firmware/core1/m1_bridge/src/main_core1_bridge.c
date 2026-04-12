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
 * Breadcrumbs (SWD-readable, survive resets as long as RAM is not cleared):
 *   0x2007FFC0  sentinel 0xC1B01200 on main() entry
 *   0x2007FFC4  rx total   — bytes drained from c0_to_c1 ring (→ CDC IN)
 *   0x2007FFC8  tx total   — bytes drained from CDC OUT (→ c1_to_c0 ring)
 *   0x2007FFCC  usb state  — bit0=mounted, bit1=cdc_connected (DTR)
 *   0x2007FFD0  loop count — bridge_task iterations (liveness)
 *
 * Placement rationale:
 *   The last 64 bytes of the .shared_ipc region (0x2007FFC0..0x20080000) lie
 *   within g_ipc_shared._tail_pad and are NOT touched by any ring traffic.
 *   Both Core 0 and Core 1 linker scripts reserve this address range as a
 *   NOLOAD shared-SRAM block, so it is immune to Core 0's FreeRTOS heap /
 *   Meshtastic task stacks — unlike the old 0x20078000 window which, despite
 *   the ipc_shared_layout.h comment, is inside Core 0's RAM region
 *   (0x20000000..0x2007A000) and gets clobbered once Meshtastic's heap
 *   usage climbs.
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

/* ── SWD breadcrumbs ─────────────────────────────────────────────────────── */
/* Last 64 B of .shared_ipc — guaranteed outside Core 0's heap & task stacks */
#define BRIDGE_SENTINEL_ADDR   0x2007FFC0u
#define BRIDGE_SENTINEL_VALUE  0xC1B01200u
#define BRIDGE_RX_COUNT_ADDR   0x2007FFC4u
#define BRIDGE_TX_COUNT_ADDR   0x2007FFC8u
#define BRIDGE_USB_STATE_ADDR  0x2007FFCCu
#define BRIDGE_LOOP_COUNT_ADDR 0x2007FFD0u

#define USB_STATE_MOUNTED      (1u << 0)
#define USB_STATE_CDC          (1u << 1)

static inline void dbg_u32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static volatile uint32_t g_rx_total;
static volatile uint32_t g_tx_total;
static volatile uint32_t g_loop_count;

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

        /* ── c0_to_c1 → CDC IN ────────────────────────────────────────── */
        IpcMsgHeader hdr;
        if (ipc_ring_pop(&g_ipc_shared.c0_to_c1_ctrl,
                         g_ipc_shared.c0_to_c1_slots,
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
                        /* No host at all (cable unplugged or device not yet
                         * enumerated). Drop the burst — there is nothing to
                         * flush into. */
                        break;
                    }
                    uint32_t avail = tud_cdc_write_available();
                    if (avail == 0u) {
                        tud_cdc_write_flush();
                        if (++stall_ticks >= 10) {
                            /* FIFO has been full for ~10 ms — assume the
                             * host isn't reading (port closed, web console
                             * paused, etc.). Drop the rest of this burst. */
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(1));
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

        /* ── CDC OUT → c1_to_c0 ───────────────────────────────────────── */
        if (tud_cdc_available()) {
            uint32_t n = tud_cdc_read(scratch, sizeof(scratch));
            if (n > 0u) {
                /* Push in one slot — if the ring is full, we spin until
                 * Core 0 drains. Typing rate on a human terminal will
                 * never fill a 32-slot ring. */
                while (!ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                                      g_ipc_shared.c1_to_c0_slots,
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

        /* Update liveness breadcrumbs. */
        g_loop_count++;
        if ((g_loop_count & 0xFFu) == 0u) {
            dbg_u32(BRIDGE_LOOP_COUNT_ADDR, g_loop_count);
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

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    /* 0. Unique boot marker — verifies over SWD that THIS build is running.
     * Written to shared IPC tail-pad (immune to both cores' crt0 zeroing).
     * Change the value each build to confirm a fresh boot. */
    dbg_u32(0x2007FFD4u, 0xDEAD0002u);  /* bump suffix each flash attempt */

    /* 1. Life signal — first thing after SDK runtime hands over. */
    dbg_u32(BRIDGE_SENTINEL_ADDR,   BRIDGE_SENTINEL_VALUE);
    dbg_u32(BRIDGE_RX_COUNT_ADDR,   0u);
    dbg_u32(BRIDGE_TX_COUNT_ADDR,   0u);
    dbg_u32(BRIDGE_USB_STATE_ADDR,  0u);
    dbg_u32(BRIDGE_LOOP_COUNT_ADDR, 0u);
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
                            IPC_MSG_LOG_LINE,
                            0u,
                            packet,
                            (uint16_t)sizeof(packet));
    }

    __atomic_store_n(&g_ipc_shared.c1_ready, 1u, __ATOMIC_RELEASE);

    /* 7. Register doorbell ISR but do NOT enable yet — enabling before the
     *    FreeRTOS scheduler starts causes a HardFault because portYIELD_FROM_ISR
     *    triggers PendSV before the port has installed the PendSV handler.
     *    The IRQ is enabled at the start of bridge_task (after scheduler runs). */
    irq_set_exclusive_handler(SIO_IRQ_BELL, ipc_doorbell_isr);

    /* 8. Hand off to FreeRTOS. */
    /* Both tasks at the same priority so taskYIELD() round-robins
     * between them without the 1 ms vTaskDelay floor. */
    xTaskCreate(usb_device_task, "usb",    1024, NULL,
                tskIDLE_PRIORITY + 2, NULL);
    xTaskCreate(bridge_task,     "bridge", 1024, NULL,
                tskIDLE_PRIORITY + 2, &g_bridge_task_handle);

    vTaskStartScheduler();

    /* Should never reach here. */
    for (;;) {
        __asm volatile ("wfi");
    }
}
