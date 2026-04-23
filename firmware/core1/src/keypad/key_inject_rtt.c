/* key_inject_rtt.c — see key_inject_rtt.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "key_inject_rtt.h"
#include "key_inject_frame.h"
#include "key_event.h"
#include "mokya_trace.h"

#include "FreeRTOS.h"
#include "task.h"

#include "SEGGER_RTT.h"

/* Room for ~36 max-sized frames before the host blocks on write. More
 * than enough for a burst of phonemes on any single-char input — a
 * worst-case CJK char is ~8 frames (phonemes + tone + commit). */
#define MOKYA_RTT_KEYINJ_BUF_BYTES   256

static uint8_t s_down_buf[MOKYA_RTT_KEYINJ_BUF_BYTES];
static TaskHandle_t s_task = NULL;

/* Stats for debug — read via SWD if a host wants to check that RTT
 * traffic is reaching the parser. */
static volatile uint32_t s_rtt_bytes_read = 0;
static volatile uint32_t s_rtt_frames_ok  = 0;
static volatile uint32_t s_rtt_rejected   = 0;
/* Debug: bumps every task iteration so a host can confirm the task is
 * actually running. */
static volatile uint32_t s_rtt_loops      = 0;
static volatile uint32_t s_rtt_has_data   = 0;

static void on_frame(uint8_t type, const uint8_t *payload, uint8_t len,
                     void *ctx)
{
    (void)ctx;
    s_rtt_frames_ok++;
    switch (type) {
    case MOKYA_KIJ_TYPE_KEY_EVENT: {
        if (len != 2u) { s_rtt_rejected++; return; }
        uint8_t ev    = payload[0];
        uint8_t flags = payload[1];
        uint8_t kc      = ev & 0x7Fu;
        int     pressed = (ev & 0x80u) ? 1 : 0;
        if (kc < 0x01u || kc >= 0x40u) { s_rtt_rejected++; return; }
        key_event_result_t r =
            key_event_push_inject_flags((mokya_keycode_t)kc, pressed, flags);
        if (r != KEY_EVENT_OK) s_rtt_rejected++;
        /* Per-keystroke TRACE removed: its vsnprintf+RTT-up-write was
         * stalling each on_frame() to ~160 ms under a burst, turning
         * a 30-frame reset_text into a 5 s drain. If debugging a
         * specific inject, enable MOKYA_KIJ_RTT_TRACE at compile time. */
        break;
    }
    case MOKYA_KIJ_TYPE_FORCE_SAVE:
        /* Cycling MODE is the lightest-touch way to trip mode_tripwire in
         * ime_task.cpp and flush the LRU to flash. Same mechanism the
         * regression script uses over SWD, without needing symbol lookup
         * on the host. */
        (void)key_event_push_inject_flags(MOKYA_KEY_MODE, 1, 0u);
        (void)key_event_push_inject_flags(MOKYA_KEY_MODE, 0, 0u);
        TRACE_BARE("kij_rtt", "force_save");
        break;
    case MOKYA_KIJ_TYPE_NOP:
        break;
    default:
        s_rtt_rejected++;
        break;
    }
}

static void key_inject_rtt_task_fn(void *arg)
{
    (void)arg;

    mokya_kij_parser_t parser;
    mokya_kij_parser_reset(&parser);

    /* 32 B scratch instead of 64: keeps task stack under 128 words.
     * A single max-sized frame is 21 B; we'll loop back around for
     * any remainder bigger than that. */
    uint8_t scratch[32];
    for (;;) {
        s_rtt_loops++;
        unsigned avail = SEGGER_RTT_HasData(MOKYA_RTT_KEYINJ_CHAN);
        s_rtt_has_data = avail;
        if (avail == 0u) {
            /* 20 ms idle poll: negligible latency vs human-speed
             * typing, but cuts the CPU footprint on otherwise-idle
             * systems so ime_task doesn't have to compete with a
             * hot-spinning RTT poller for the sub-priority band. */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        unsigned got = SEGGER_RTT_Read(MOKYA_RTT_KEYINJ_CHAN,
                                       scratch, sizeof(scratch));
        if (got == 0u) {
            /* HasData said >0 but Read returned 0 — transient race with
             * a simultaneous host rtt_write. Yield to avoid a tight
             * loop burning CPU. */
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        s_rtt_bytes_read += got;
        mokya_kij_parser_push(&parser, scratch, got, on_frame, NULL);
    }
}

void key_inject_rtt_task_start(void)
{
    if (s_task) return;
    SEGGER_RTT_ConfigDownBuffer(MOKYA_RTT_KEYINJ_CHAN, "keyinj",
                                s_down_buf, sizeof(s_down_buf),
                                SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    /* 256-word (1024 B) stack. 128 was enough for the bare loop but
     * overflows inside TRACE() — that macro stack-allocates a 128 B
     * vsnprintf scratch on top of parser + SEGGER_RTT_Read frames.
     * Overflow was silent (task stops after exactly 1 frame). 256 words
     * stays within the 15 %-heap-reserve budget (see main_core1_bridge).
     *
     * Priority +2, same as the SWD key_inject task. +3 was faster on
     * raw drain but starved ime when both inject transports sat at
     * the same priority as ime — candidate list stayed empty, no
     * IME state change. +2 relies on ime blocking often enough on
     * key_event_pop() to let us in. For burst-heavy regression runs
     * this gives ~100 frames/s throughput (still 10x SWD). */
    xTaskCreate(key_inject_rtt_task_fn, "key_inj_rtt",
                256, NULL, tskIDLE_PRIORITY + 2, &s_task);
    (void)on_frame;
}
