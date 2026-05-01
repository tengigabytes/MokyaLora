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
#include "hardware/watchdog.h"
#include "tusb.h"
#include "pico/platform/panic.h"

#include "FreeRTOS.h"
#include "task.h"

#include "ipc_protocol.h"
#include "ipc_shared_layout.h"
#include "ipc_ringbuf.h"

#include "i2c_bus.h"
#include "bq25622.h"
#include "sensor_task.h"
#include "gps_task.h"
#include "display.h"
#include "lvgl_glue.h"
#include "keypad_scan.h"
#include "key_event.h"
#include "key_inject.h"
#include "key_inject_rtt.h"
#include "messages_tx_status.h"
#include "dm_store.h"
#include "dm_persist.h"
#include "c1_storage.h"
#include "waypoint_persist.h"
#include "settings_client.h"
#include "watchdog_task.h"
#include "history.h"
#ifdef MOKYA_PHONEAPI_CASCADE
#include "phoneapi_session.h"
#endif
#include "postmortem.h"
#include "msp_canary.h"

volatile uint32_t g_core1_boot_heap_free = 0;
#include "psram.h"
#include "mie_dict_loader.h"
#include "ime_task.h"

#include "mokya_trace.h"

/* Dict pointers stay file-scope; ime_task_start takes a pointer to it. */
static mie_dict_pointers_t s_mie_dict = {0};

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

/* ── USB-host active detection ─────────────────────────────────────────────
 * Cascade-PhoneAPI is the primary client (Core 1 is host). USB CDC is a
 * forward sub-mode that engages ONLY when a real external host is present.
 *
 * "Real host" is the OR of:
 *   - tud_cdc_connected()  — DTR asserted (pyserial, `meshtastic` CLI, …)
 *   - recent CDC OUT activity — host has sent bytes within the grace
 *     window (Chrome WebSerial, which doesn't assert DTR but does send
 *     want_config_id at session start, falls into this branch)
 *
 * Without either signal, USB write is skipped entirely; cascade has
 * already consumed the byte slot, so there is no data loss internally,
 * and no spinning trying to write to a FIFO no one will drain. */
#define CDC_ACTIVE_GRACE_MS  5000u
static volatile TickType_t s_cdc_last_active_tick = 0;

static inline void cdc_mark_active(void)
{
    s_cdc_last_active_tick = xTaskGetTickCount();
}

static bool cdc_host_active(void)
{
    if (tud_cdc_connected()) return true;
    TickType_t last = s_cdc_last_active_tick;
    if (last == 0) return false;
    TickType_t now = xTaskGetTickCount();
    return (TickType_t)(now - last) < pdMS_TO_TICKS(CDC_ACTIVE_GRACE_MS);
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

    /* Cascade session FORWARD/STANDALONE follows cdc_host_active(), the
     * authoritative liveness signal that combines DTR (USB CDC default)
     * with recent CDC OUT activity (covers Chrome WebSerial which never
     * raises DTR). M5E.4 — replaces the raw tud_cdc_line_state_cb hook
     * that only saw DTR. */
    bool prev_cdc_active = false;

    /* Enable doorbell IRQ now that the FreeRTOS scheduler is running and
     * PendSV/SVC handlers are installed.  Any pending doorbells from
     * Core 0 during the pre-scheduler busy-wait will fire immediately. */
    irq_set_enabled(SIO_IRQ_BELL, true);

    /* Phase 2 — c1_storage init + selftest run here (post-scheduler) so
     * the LFS lock callback can take the FreeRTOS recursive mutex. The
     * selftest writes diag globals readable via SWD; failure is non-
     * fatal — DM persist consumers in Phase 3 fall back to RAM-only if
     * c1_storage_is_mounted() returns false.  TEMPORARY: disabled while
     * debugging early-boot Core 1 hang during lfs_format on fresh chip.
     * Re-enable after root-cause fix. */
    (void)c1_storage_init();
    (void)c1_storage_self_test();
    /* Phase 3 — DM persistence. Loads any previously-saved peer files
     * into dm_store and starts the 30 s flush timer. Safe even if
     * c1_storage failed to mount (load returns 0, timer skipped). */
    (void)dm_persist_load_all();
    dm_persist_init();
    /* Phase 4 — waypoint persistence. Same shape, single /.waypoints
     * file. Independent 30 s timer. */
    (void)waypoint_persist_load_all();
    waypoint_persist_init();

    for (;;) {
        /* If Core 0 announced a reboot, stop all ring/CDC processing and
         * idle until the watchdog fires the chip-wide reset. */
        if (reboot_pending) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Drive cascade FSM from cdc_host_active(). Edge-triggered so
         * phoneapi_session_set_usb_connected() only runs when state
         * actually changes. */
        bool now_cdc_active = cdc_host_active();
        if (now_cdc_active != prev_cdc_active) {
            phoneapi_session_set_usb_connected(now_cdc_active);
            prev_cdc_active = now_cdc_active;
        }

        bool did_work = false;

        /* ── Phase 2.6 watchdog reset trigger (SWD-driven, test only) ── *
         *
         * Test scripts SWD-write a non-zero value to request a chip-
         * wide watchdog reboot. Used by power-loss survival test —
         * RP2350 SYSRESETREQ via JLink halts only one core; only
         * watchdog forces full bootrom-driven cold-start of both cores. */
        {
            extern volatile uint32_t g_c1_storage_reset_request;
            if (g_c1_storage_reset_request != 0u) {
                /* Pause watchdog kicks; chip resets in 100 ms. */
                watchdog_enable(100, 1);
                while (1) { tight_loop_contents(); }
            }
        }

        /* ── Format-FS trigger (SWD-driven, test only) ── */
        {
            extern volatile uint32_t g_c1_storage_format_request;
            extern volatile uint32_t g_c1_storage_format_done;
            uint32_t req = g_c1_storage_format_request;
            if (req != 0u && req != g_c1_storage_format_done) {
                (void)c1_storage_format_now();
                g_c1_storage_format_done = req;
                did_work = true;
            }
        }

        /* ── DM persist flush trigger (SWD-driven, test only) ── */
        {
            extern volatile uint32_t g_dm_persist_flush_request;
            extern volatile uint32_t g_dm_persist_flush_done;
            uint32_t req = g_dm_persist_flush_request;
            if (req != 0u && req != g_dm_persist_flush_done) {
                (void)dm_persist_flush_now();
                g_dm_persist_flush_done = req;
                did_work = true;
            }
        }

        /* ── Waypoint persist flush trigger (SWD-driven, test only) ── */
        {
            extern volatile uint32_t g_waypoint_persist_flush_request;
            extern volatile uint32_t g_waypoint_persist_flush_done;
            uint32_t req = g_waypoint_persist_flush_request;
            if (req != 0u && req != g_waypoint_persist_flush_done) {
                (void)waypoint_persist_flush_now();
                g_waypoint_persist_flush_done = req;
                did_work = true;
            }
        }

        /* ── Phase 2.5 capacity stress trigger (SWD-driven, default OFF) ── *
         *
         * Test scripts SWD-write a non-zero value to g_c1_storage_stress_request
         * encoded as `(n_files << 16) | bytes_per_file_in_256B_units`.
         * Firmware acks by mirroring the request value into _stress_done.
         * Cheap poll — single uint32 read per loop iteration. */
        {
            extern volatile uint32_t g_c1_storage_stress_request;
            extern volatile uint32_t g_c1_storage_stress_done;
            uint32_t req = g_c1_storage_stress_request;
            if (req != 0u && req != g_c1_storage_stress_done) {
                uint32_t n_files = (req >> 16) & 0xFFFFu;
                uint32_t bytes   = (req & 0xFFFFu) * 256u;
                (void)c1_storage_stress_test(n_files, bytes);
                g_c1_storage_stress_done = req;
                did_work = true;
            }
        }

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

            if (hdr.msg_id == IPC_MSG_CONFIG_VALUE ||
                hdr.msg_id == IPC_MSG_CONFIG_RESULT) {
                settings_client_dispatch_reply(hdr.msg_id,
                                               scratch,
                                               hdr.payload_len);
                did_work = true;
                continue;
            }

            /* M5E.3: RX_TEXT / NODE_UPDATE / TX_ACK dispatch arms removed.
             * Cascade (phoneapi_session) is now the sole source for those
             * events, decoding them out of the FromRadio byte stream that
             * arrives via IPC_MSG_SERIAL_BYTES. The enum IDs themselves
             * are retired in ipc_protocol.h. nodes_view temporarily
             * reads stale data until M5F.2 migrates it to
             * phoneapi_cache_take_node_at(). */

            if (hdr.msg_id == IPC_MSG_SERIAL_BYTES && hdr.payload_len > 0u) {
#ifdef MOKYA_PHONEAPI_CASCADE
                /* Cascade tap: parse FromRadio frames in parallel with the
                 * existing CDC pass-through. The framing parser is
                 * stateful across slots, so feed the entire payload here
                 * before any USB write — pass-through bytes are NOT
                 * modified by this call. */
                phoneapi_session_feed_from_core0(scratch, hdr.payload_len);
#endif
                uint16_t remaining = hdr.payload_len;
                const uint8_t *p = scratch;
                /* The data ring carries Meshtastic stream-protocol frames
                 * (0x94 0xC3 LEN_HI LEN_LO header + LEN payload bytes).
                 * The host's parser reads exactly LEN payload bytes after
                 * the header; if we drop ANY byte mid-frame here, every
                 * subsequent frame on the wire desynchronises (host reads
                 * into the next frame's header, parse fails, alignment
                 * lost). There is no frame-level retransmit. So when CDC
                 * FIFO is full, keep yielding until the host drains it —
                 * never abort mid-payload. The only legitimate way to
                 * stop is if USB unmounts (host gone). */
                /* USB pass-through engages only when a real external host
                 * is present (DTR asserted OR recent CDC OUT activity).
                 * Without one, this slot's bytes have already been
                 * consumed by the cascade tap above — no need to push to
                 * a FIFO no one will drain. Strict gate, no spinning. */
                if (cdc_host_active() && tud_mounted()) {
                    while (remaining > 0u) {
                        if (!tud_mounted() || !cdc_host_active()) {
                            /* Host went away mid-write. Stop, cascade
                             * already has all bytes, abandoning the USB
                             * pass-through here may break alignment for
                             * the just-departed host but they aren't
                             * reading anymore. */
                            break;
                        }
                        uint32_t avail = tud_cdc_write_available();
                        if (avail == 0u) {
                            /* Transient backpressure from an active host
                             * — yield until they drain. cdc_host_active
                             * keeps this bounded. */
                            tud_cdc_write_flush();
                            taskYIELD();
                            continue;
                        }
                        uint32_t chunk = (remaining < avail) ? remaining : avail;
                        uint32_t wrote = tud_cdc_write(p, chunk);
                        p += wrote;
                        remaining -= (uint16_t)wrote;
                        g_rx_total += wrote;
                    }
                }
                tud_cdc_write_flush();
                dbg_u32(BRIDGE_RX_COUNT_ADDR, g_rx_total);
            }
            did_work = true;
        }

        /* ── c0_log_to_c1 LOG ring → DROPPED (P2-19) ──────────────────
         * Forwarding Meshtastic LOG output onto the same USB CDC IN
         * endpoint as binary protobuf frames is unsafe even with the
         * "data-ring-empty" gate above: between two ring-slot pushes
         * for the same multi-chunk FromRadio frame, Core 0's chunking
         * loop briefly leaves the data ring empty (gap is sub-µs but
         * non-zero — Core 1 runs on its own clock and can iterate the
         * bridge loop in that window). When that window happens to
         * coincide with a `\n` flush of an accumulated log line, the
         * else-if branch drains a log slot and writes its bytes
         * mid-frame, breaking host stream alignment.
         *
         * Symptom: rare, traffic-pattern-dependent FromRadio parse
         * errors. Settings-page entry on client.meshtastic.org
         * triggers it reliably because the get_config sequence pairs
         * a burst of admin protobuf frames with a burst of
         * "Handle admin payload" / "Send config" log lines — UI
         * spins waiting for a config response that gets corrupted.
         *
         * Drop log bytes here. Bridge task does NOT set did_work, so
         * it sleeps on xTaskNotifyWait when only log work is present
         * — this prevents the tight log-drain loop from starving the
         * USB device task (`tud_task()` must run to actually push
         * queued CDC IN bytes onto the wire).
         *
         * Logs are still available via SEGGER RTT (mokya_trace.h
         * shares the same RTT control block). For Meshtastic LOG_*
         * specifically, future M9 USB Control Interface (DEC-2) can
         * carve out a separate CDC interface for log output. */
        else if (ipc_ring_pop(&g_ipc_shared.c0_log_to_c1_ctrl,
                               g_ipc_shared.c0_log_to_c1_slots,
                               IPC_LOG_RING_SLOT_COUNT,
                               &hdr,
                               scratch,
                               sizeof(scratch))) {
            (void)hdr;  /* drop silently; do NOT set did_work */
        }

        /* ── CDC OUT → c1_to_c0 CMD ring ──────────────────────────────── */
        if (tud_cdc_available()) {
            uint32_t n = tud_cdc_read(scratch, sizeof(scratch));
            if (n > 0u) {
                /* Host sent bytes — confirms they're actively engaged.
                 * Refreshes the cdc_host_active() grace window so USB
                 * pass-through stays enabled even if DTR isn't set
                 * (Chrome WebSerial). */
                cdc_mark_active();
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

        /* Fault injector for postmortem regression testing. No-op
         * unless the SWD host has set g_mokya_pm_test_force_fault. */
        mokya_pm_test_poll();

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
    /* MSP canary fill — must run before any deep call chain. The .heap
     * region (~2 KB) sitting between .bss top and __StackTop has no
     * other consumer in this image, so we paint it with 0xDEADBEEF and
     * later scan to discover the historical MSP low-water mark. See
     * docs/design-notes/core1-memory-budget.md §1 (MSP guard). */
    msp_canary_init();

    /* Zero operational counters in the tail pad so SWD sees 0 before the
     * bridge task starts publishing real values. */
    dbg_u32(BRIDGE_RX_COUNT_ADDR,   0u);
    dbg_u32(BRIDGE_TX_COUNT_ADDR,   0u);
    dbg_u32(BRIDGE_USB_STATE_ADDR,  0u);
    __dmb();

    /* RTT trace transport up — see mokya_trace.h. The control block lives
     * at &_SEGGER_RTT in BSS; J-Link / OpenOCD / pyOCD all auto-discover it.
     * Init must run before any TRACE() call. Cheap (~50 instructions),
     * does not touch hardware, safe to call before the boot_magic spin. */
    SEGGER_RTT_Init();
    TRACE_BARE("core1", "boot");

    /* If the previous reset left a postmortem snapshot in shared SRAM
     * (cause = WD_SILENT / HARDFAULT / etc.), print it now and clear
     * the magic so subsequent boots don't re-log the same event. */
    mokya_pm_surface_on_boot();

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

    /* PSRAM (APS6404L, 8 MB at 0x11000000). Done before any driver so
     * allocations that want PSRAM can succeed from task-start. Core 0
     * does not initialise PSRAM (Arduino-Pico's psram.cpp is gated on
     * RP2350_PSRAM_CS which we do not define), so Core 1 owns it. The
     * read-ID result lands in g_psram_read_id[] for SWD inspection;
     * failure here is logged but non-fatal (IME will panic later when
     * it tries to copy the dict blob). */
    (void)psram_init();

    /* Zero the .psram_bss section. PSRAM is not part of crt0's
     * __bss_start__/__bss_end__ range, so anything placed in PSRAM via
     * `__attribute__((section(".psram_bss")))` retains whatever the
     * APS6404L came up with after reset. We do it once here before any
     * consumer can access it. Empty section is fine — start == end. */
    {
        extern uint32_t __psram_bss_start;
        extern uint32_t __psram_bss_end;
        uint32_t *p   = &__psram_bss_start;
        uint32_t *end = &__psram_bss_end;
        while (p < end) *p++ = 0u;
    }

    /* Copy the MDBL dict blob from flash partition (0x10400000) to
     * PSRAM. Status in g_mie_dict_load_status; on failure s_mie_dict
     * is zeroed and the future ime_task will refuse to start. */
    (void)mie_dict_load_to_psram(&s_mie_dict);


    /* Shared I2C bus (time-muxed i2c1). Creates s_bus_mutex and sets
     * the default pinmux to the POWER pair. MUST run before any task
     * that calls i2c_bus_acquire(): lvgl_task (lm27965 backlight),
     * charger_task, sensor_task, gps_task. The call was accidentally
     * dropped in M4 Step 3 when PSRAM init was added — restored here. */
    i2c_bus_init_all();

    /* 7. Register doorbell ISR but do NOT enable yet — enabling before the
     *    FreeRTOS scheduler starts causes a HardFault because portYIELD_FROM_ISR
     *    triggers PendSV before the port has installed the PendSV handler.
     *    The IRQ is enabled at the start of bridge_task (after scheduler runs). */
    irq_set_exclusive_handler(SIO_IRQ_BELL, ipc_doorbell_isr);

    /* 8. Hand off to FreeRTOS.
     *
     * Every task-start is checked. Silent failures here caused a whole
     * class of "HardFault in vTaskStartScheduler" debugging sessions —
     * see phase2-log.md M3.4.5d. The panic message names the failing
     * task so the USB CDC log points straight at the culprit.
     *
     * Heap budget is documented in docs/design-notes/core1-memory-budget.md;
     * keep that file and the stack depths here in sync. */
    #define TASK_START_OR_PANIC(expr, name) \
        do { if (!(expr)) { panic("core1: " name " task_start failed"); } } while (0)

    /* Both tasks at the same priority so taskYIELD() round-robins
     * between them without the 1 ms vTaskDelay floor. */
    TASK_START_OR_PANIC(
        xTaskCreate(usb_device_task, "usb", 1024, NULL,
                    tskIDLE_PRIORITY + 2, NULL) == pdPASS,
        "usb");
    TASK_START_OR_PANIC(
        xTaskCreate(bridge_task, "bridge", 1024, NULL,
                    tskIDLE_PRIORITY + 2, &g_bridge_task_handle) == pdPASS,
        "bridge");

    /* LVGL service task runs at the same priority as usb / bridge so the
     * round-robin scheduler hands it CPU between their yields. A lower
     * priority would be starved by usb_device_task's tight tud_task()
     * + taskYIELD() loop. lvgl_task owns display_init() — main() must not
     * also call it. */
    TASK_START_OR_PANIC(lvgl_glue_start(tskIDLE_PRIORITY + 2), "lvgl");

    /* KeyEvent queue — allocate before creating keypad_scan_task so the
     * very first debounce commit (possible within ~20 ms of boot if a
     * key is held) finds the queue ready. Producer/consumer are both
     * future tasks; only the scan task enqueues in Phase B. */
    key_event_init();

    /* Per-peer DM store (Phase 3). Pure BSS reset; mutex created lazily
     * by the first writer call after the scheduler is up. */
    dm_store_init();
    /* c1_storage_init() + selftest deferred to bridge_task setup —
     * LFS lock callback uses xSemaphoreCreateRecursiveMutex which
     * requires the FreeRTOS scheduler to be running.  Calling here
     * (pre-scheduler) traps in vTaskSuspendAll. */

    /* Settings reply queue — created before bridge_task starts dispatching
     * IPC_MSG_CONFIG_VALUE / IPC_MSG_CONFIG_RESULT into it. */
    settings_client_init();

#ifdef MOKYA_PHONEAPI_CASCADE
    /* Cascade PhoneAPI byte-stream tap (M5 Phase 2 Phase A). Initialised
     * before bridge_task starts so the very first SERIAL_BYTES slot is
     * fed into the framing parser. */
    phoneapi_session_init();
#endif

    /* T2.6 — F-4 trend ring sampler (30 s soft timer). Safe to start now;
     * BQ25622 is up from sensor_task init and the cascade RX hook can
     * begin storing into the SNR slot as soon as packets arrive. */
    metrics_history_init();

    /* Keypad scan task — MUST run at the same priority as usb / bridge / lvgl.
     * usb_device_task is a `tud_task(); taskYIELD();` loop with no blocking
     * call, so it is always Ready at priority tskIDLE_PRIORITY + 2. Under
     * preemptive scheduling, any strictly-lower-priority task is starved.
     * Equal priority enables round-robin time-slicing, giving keypad its
     * slot between the 5 ms debounce pauses. 512-word stack is plenty
     * for the loop with 6-byte local raw buffer and no nested calls. */
    TASK_START_OR_PANIC(
        xTaskCreate(keypad_scan_task, "kpad", 512, NULL,
                    tskIDLE_PRIORITY + 2, NULL) == pdPASS,
        "kpad");

    /* Charger (BQ25622) 1 Hz poll task — hw init runs inside the task so
     * first I2C traffic is post-scheduler, after i2c_bus mutex is usable. */
    TASK_START_OR_PANIC(bq25622_start_task(tskIDLE_PRIORITY + 2), "chg");

    /* Sensor bus poll task (M3.4.5). LPS22HH + LIS2MDL + LSM6DSV16X on
     * a shared 10 Hz tick with per-sensor divider counters. */
    TASK_START_OR_PANIC(sensor_task_start(tskIDLE_PRIORITY + 2), "sens");

    /* GNSS task (M3.4.5d). Runs separately from sensor_task — NMEA drain
     * cadence is 100 ms, faster than the 1/10 Hz sensor tick, and the
     * line-accumulator parser carries state between ticks. */
    TASK_START_OR_PANIC(gps_task_start(tskIDLE_PRIORITY + 2), "gps");

    /* IME task (M4). Consumes the KeyEvent queue, drives mie::ImeLogic,
     * and exposes composition / candidate / commit state for the LVGL
     * view via the ime_view_* getter API. Priority +3 one notch above
     * the other app tasks so keystrokes get processed before the next
     * LVGL frame. If the dict copy-at-boot failed the pointers are
     * zeroed and ime_task_start returns false → panic, which is the
     * right behaviour (IME unusable means nothing to do). */
    TASK_START_OR_PANIC(ime_task_start(&s_mie_dict, tskIDLE_PRIORITY + 3),
                        "ime");

    /* SWD key-injection task — lets a J-Link-equipped host write virtual
     * keypress events into g_key_inject_buf so the IME can be driven
     * programmatically without a human at the keypad. Arbitration ensures
     * the user's physical keypress always wins. Safe in production: if
     * nobody writes to the ring, the task just polls and sleeps. */
    key_inject_task_start();
    /* RTT alternate transport. Both inject tasks coexist but only the
     * one selected by g_key_inject_mode actively polls — the other
     * long-sleeps (50 ms) so ime_task doesn't compete with two hot
     * pollers. Host flips the mode byte via SWD for the duration of
     * a RTT burst and flips it back when done. Default = SWD.        */
    key_inject_rtt_task_start();

    /* Watchdog task — owns the HW watchdog. Priority +3 (same as ime)
     * so it is never starved by the round-robin app tasks at +2. The
     * task itself enables the HW watchdog on its first iteration; until
     * then no kicks are due. See watchdog_task.h for the hang model. */
    TASK_START_OR_PANIC(watchdog_task_start(tskIDLE_PRIORITY + 3), "wd");

    #undef TASK_START_OR_PANIC

    /* Heap budget checkpoint (see docs/design-notes/core1-memory-budget.md §5).
     * configTOTAL_HEAP_SIZE is 48 KB. Original policy was ≥ 20 % reserve;
     * loosened to 15 % when Phase 1.6.1's RTT key-inject task pushed
     * free heap down to ~9 KB; loosened again to 14 % when B2 Stage 2
     * (settings UI, ~640 B) and the watchdog task (~860 B) brought
     * boot free heap down to ~7.3 KB. 14 % = 6.7 KB reserve is still
     * comfortably above runtime transient peaks. Bumping
     * configTOTAL_HEAP_SIZE is not viable on Rev A — RAM region is full
     * (.heap section already abuts .shared_ipc at 0x2007A000). */
    size_t heap_total = configTOTAL_HEAP_SIZE;
    size_t heap_free  = xPortGetFreeHeapSize();
    size_t heap_min   = (heap_total * 14) / 100;   /* 14 % reserve */
    /* Expose heap_free as a file-static so the boot value can be read
     * via SWD when a panic appears to fire silently. */
    extern volatile uint32_t g_core1_boot_heap_free;
    g_core1_boot_heap_free = (uint32_t)heap_free;
    if (heap_free < heap_min) {
        panic("core1: heap reserve below 20%% — used=%u / total=%u",
              (unsigned)(heap_total - heap_free), (unsigned)heap_total);
    }

    vTaskStartScheduler();

    /* Should never reach here. */
    for (;;) {
        __asm volatile ("wfi");
    }
}
