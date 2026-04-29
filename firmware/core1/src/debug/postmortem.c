/* postmortem.c — see postmortem.h. */

#include "postmortem.h"

#include <string.h>

#include "hardware/timer.h"
#include "hardware/structs/scb.h"

#include "FreeRTOS.h"
#include "task.h"

#include "ipc_shared_layout.h"
#include "mokya_trace.h"

/* SWD-pokeable test trigger — when set non-zero by a debugger, the
 * tick polled by mokya_pm_test_poll() forces a BusFault (load from an
 * unmapped XIP address). Lets us validate the fault-handler path on
 * demand without requiring a real bug. Default 0 → no behaviour. */
volatile uint32_t g_mokya_pm_test_force_fault = 0;

void mokya_pm_test_poll(void)
{
    if (g_mokya_pm_test_force_fault == 0u) return;
    g_mokya_pm_test_force_fault = 0u;
    /* `udf #0` is the ARMv8-M permanently-undefined instruction. It
     * raises an UNDEFINSTR fault which escalates to UsageFault (or
     * HardFault if UsageFault isn't enabled). Reliable reproduction
     * across Cortex-M33 cores regardless of MPU / memory layout. */
    __asm volatile ("udf #0" ::: "memory");
}

/* AIRCR SYSRESETREQ — bypasses watchdog scratch (BOOTSEL recovery
 * uses SCRATCH4..7, see reference_qmi_wedge_swd_recovery memory). */
#define MOKYA_AIRCR_ADDR    ((volatile uint32_t *)0xE000ED0Cu)
#define MOKYA_AIRCR_VECTKEY 0x05FA0000u
#define MOKYA_AIRCR_RESET   (MOKYA_AIRCR_VECTKEY | (1u << 2))

/* SCB fault status registers (CFSR is single 32-bit aggregate of
 * MMFSR | BFSR<<8 | UFSR<<16). Address from ARM Cortex-M33 ARM. */
#define MOKYA_SCB_CFSR  ((volatile uint32_t *)0xE000ED28u)
#define MOKYA_SCB_HFSR  ((volatile uint32_t *)0xE000ED2Cu)
#define MOKYA_SCB_MMFAR ((volatile uint32_t *)0xE000ED34u)
#define MOKYA_SCB_BFAR  ((volatile uint32_t *)0xE000ED38u)

/* Best-effort "current task name" — only valid once the FreeRTOS
 * scheduler has started. Returns "" otherwise. The pcTaskGetName(NULL)
 * call is technically not ISR-safe, but it just walks the active TCB
 * pointer and reads a fixed-size char array — no critical section
 * required. We intentionally call it from the fault handler context
 * to capture which task was running when the fault hit. */
static void copy_task_name(char *dst)
{
    dst[0] = '\0';
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return;
    TaskHandle_t h = xTaskGetCurrentTaskHandle();
    if (h == NULL) return;
    const char *n = pcTaskGetName(h);
    if (n == NULL) return;
    for (uint8_t i = 0; i < MOKYA_PM_TASK_NAME_MAX - 1; ++i) {
        char c = n[i];
        if (c == '\0') break;
        dst[i] = c;
    }
}

void mokya_pm_snapshot_silent(uint32_t c0_heartbeat,
                              uint32_t wd_state,
                              uint32_t wd_silent_max,
                              uint32_t wd_pause)
{
    mokya_postmortem_t *pm = &g_ipc_shared.postmortem_c1;
    /* First-event-wins. If a fault handler already filled this in we
     * don't want to overwrite the more useful CPU context. */
    if (pm->magic == MOKYA_PM_MAGIC) return;

    memset(pm, 0, sizeof(*pm));
    pm->cause         = (uint32_t)MOKYA_PM_CAUSE_WD_SILENT;
    pm->timestamp_us  = timer_hw->timerawl;
    pm->core          = 1;
    pm->c0_heartbeat  = c0_heartbeat;
    pm->wd_state      = wd_state;
    pm->wd_silent_max = wd_silent_max;
    pm->wd_pause      = wd_pause;
    copy_task_name(pm->task_name);
    /* Publish magic last so a partially-written record never appears
     * valid to the surface path. */
    __atomic_store_n(&pm->magic, MOKYA_PM_MAGIC, __ATOMIC_RELEASE);
}

/* Common fault path. `frame` points at the 8-word stacked exception
 * frame on whichever stack was active at fault entry; `cause` is one
 * of MOKYA_PM_CAUSE_*; `exc_return` is the value of LR at handler
 * entry (the EXC_RETURN magic). After capture, force a chip reset. */
__attribute__((noreturn, used))
void mokya_pm_fault_capture(uint32_t *frame, uint32_t cause,
                             uint32_t exc_return)
{
    mokya_postmortem_t *pm = &g_ipc_shared.postmortem_c1;
    /* Always overwrite — fault info is the most valuable. */
    memset(pm, 0, sizeof(*pm));
    pm->cause        = cause;
    pm->timestamp_us = timer_hw->timerawl;
    pm->core         = 1;

    pm->r0  = frame[0];
    pm->r1  = frame[1];
    pm->r2  = frame[2];
    pm->r3  = frame[3];
    pm->r12 = frame[4];
    pm->lr  = frame[5];
    pm->pc  = frame[6];
    pm->psr = frame[7];
    pm->sp  = (uint32_t)frame;
    pm->exc_return = exc_return;

    pm->cfsr  = *MOKYA_SCB_CFSR;
    pm->hfsr  = *MOKYA_SCB_HFSR;
    pm->mmfar = *MOKYA_SCB_MMFAR;
    pm->bfar  = *MOKYA_SCB_BFAR;

    pm->c0_heartbeat = __atomic_load_n(&g_ipc_shared.c0_heartbeat,
                                       __ATOMIC_RELAXED);
    pm->wd_pause     = __atomic_load_n(&g_ipc_shared.wd_pause,
                                       __ATOMIC_RELAXED);
    /* Don't pull g_wd_state symbol here — wd_task lives in another TU
     * and may have been corrupted; the caller (wd_task) writes its
     * own snapshot path for non-fault causes. */
    copy_task_name(pm->task_name);

    /* Stack snapshot — copy 32 words (128 B) starting at the active SP.
     * Cortex-M stack descends, so frame[0..7] is the lowest-address
     * exception frame; addresses ABOVE frame are older stack content
     * (caller frames, locals, return addresses). Bounds-check against
     * the SRAM end (0x20082000 on RP2350) so a near-overflow stack
     * doesn't wander into unmapped memory and re-fault inside the
     * handler. Words past the available range stay zero (memset above). */
    {
        const uint32_t k_sram_end = 0x20082000u;
        uint32_t *src = frame;
        uint32_t avail_bytes = (k_sram_end > (uint32_t)src)
                               ? (k_sram_end - (uint32_t)src) : 0u;
        uint32_t avail_words = avail_bytes / 4u;
        uint32_t want = sizeof(pm->stack) / sizeof(pm->stack[0]);
        uint32_t n = (avail_words < want) ? avail_words : want;
        for (uint32_t i = 0; i < n; ++i) pm->stack[i] = src[i];
        pm->stack_words = (uint16_t)n;
    }

    __atomic_store_n(&pm->magic, MOKYA_PM_MAGIC, __ATOMIC_RELEASE);

    /* Reset the chip. SYSRESETREQ also clears Core 1 fully; the
     * shared SRAM postmortem slot is preserved across this reset. */
    *MOKYA_AIRCR_ADDR = MOKYA_AIRCR_RESET;
    for (;;) { __asm volatile ("dsb 0xF; isb"); }
}

/* Naked exception entry stubs. Pass the active stack frame (MSP or
 * PSP depending on EXC_RETURN bit 2) plus the cause + EXC_RETURN to
 * the C capture function via the AAPCS argument registers. */

#define MOKYA_PM_FAULT_STUB(name, cause)                          \
    __attribute__((naked, used))                                  \
    void name(void)                                               \
    {                                                             \
        __asm volatile (                                          \
            "mov r2, lr                  \n"                      \
            "tst lr, #4                  \n"                      \
            "ite eq                      \n"                      \
            "mrseq r0, msp               \n"                      \
            "mrsne r0, psp               \n"                      \
            "movs r1, %[cs]              \n"                      \
            "b mokya_pm_fault_capture    \n"                      \
            :: [cs] "i" (cause)                                   \
        );                                                        \
    }

MOKYA_PM_FAULT_STUB(isr_hardfault,    MOKYA_PM_CAUSE_HARDFAULT)
MOKYA_PM_FAULT_STUB(isr_memmanage,    MOKYA_PM_CAUSE_MEMMANAGE)
MOKYA_PM_FAULT_STUB(isr_busfault,     MOKYA_PM_CAUSE_BUSFAULT)
MOKYA_PM_FAULT_STUB(isr_usagefault,   MOKYA_PM_CAUSE_USAGEFAULT)

/* ── Boot-time surface ─────────────────────────────────────────────── */

static void surface_one(const char *slot_name, mokya_postmortem_t *pm)
{
    if (pm->magic != MOKYA_PM_MAGIC) return;

    TRACE("pm", slot_name,
          "cause=%u t=%u pc=0x%08x lr=0x%08x sp=0x%08x psr=0x%08x "
          "cfsr=0x%08x hfsr=0x%08x mmfar=0x%08x bfar=0x%08x "
          "hb=%u wd_state=0x%08x silent_max=%u pause=%u task=%s",
          (unsigned)pm->cause, (unsigned)pm->timestamp_us,
          (unsigned)pm->pc, (unsigned)pm->lr, (unsigned)pm->sp,
          (unsigned)pm->psr,
          (unsigned)pm->cfsr, (unsigned)pm->hfsr,
          (unsigned)pm->mmfar, (unsigned)pm->bfar,
          (unsigned)pm->c0_heartbeat, (unsigned)pm->wd_state,
          (unsigned)pm->wd_silent_max, (unsigned)pm->wd_pause,
          pm->task_name);

    /* Stack snapshot — emit in 4-word chunks so each TRACE line stays
     * readable. Walk addr in ascending order (that's how the array was
     * captured: stack[0] = SRAM at SP, stack[N] = SP + 4N). Words
     * with values that look like thumb code addresses (low half of
     * flash, bit 0 set) are likely return addresses — the analyser
     * can post-process. Skip if no words captured. */
    if (pm->stack_words > 0u) {
        for (uint16_t i = 0; i < pm->stack_words; i += 4u) {
            uint32_t w0 = pm->stack[i];
            uint32_t w1 = (i + 1 < pm->stack_words) ? pm->stack[i + 1] : 0u;
            uint32_t w2 = (i + 2 < pm->stack_words) ? pm->stack[i + 2] : 0u;
            uint32_t w3 = (i + 3 < pm->stack_words) ? pm->stack[i + 3] : 0u;
            TRACE("pm", "stk",
                  "off=0x%02x %08x %08x %08x %08x",
                  (unsigned)(i * 4u),
                  (unsigned)w0, (unsigned)w1, (unsigned)w2, (unsigned)w3);
        }
    }

    /* Clear magic so this event is logged at most once. The rest of
     * the slot stays intact for SWD inspection. */
    __atomic_store_n(&pm->magic, 0u, __ATOMIC_RELEASE);
}

void mokya_pm_surface_on_boot(void)
{
    /* Surface both cores' slots — Core 1 has reliable RTT this early
     * in boot, Core 0 doesn't (Serial isn't enumerated yet at the
     * equivalent point in initVariant). Core 0's snapshot writer
     * leaves the slot for us to read here. */
    surface_one("c0_last", &g_ipc_shared.postmortem_c0);
    surface_one("c1_last", &g_ipc_shared.postmortem_c1);
}
