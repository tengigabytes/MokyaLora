/* MokyaLora Phase 2 M1.1-B — Core 1 m1_bridge linker script
 *
 * Derived from pico-sdk/src/rp2_common/pico_crt0/rp2350/memmap_default.ld but
 * re-targeted for the multicore_launch_core1_raw() boot model:
 *
 *   - FLASH origin moves from 0x10000000 to 0x10200000 (2 MB region carved
 *     out of W25Q128JW, matches docs/requirements/software-requirements.md
 *     §1.2 Core 1 firmware slot).
 *   - RAM origin moves from 0x20000000 to 0x20040000 (64 KB carve-out, leaves
 *     Core 0 the first 256 KB and leaves 0x20050000..0x20078000 as unused
 *     slack for future heap growth on either side).
 *   - No SCRATCH_X / SCRATCH_Y carve: those addresses (0x20080000..0x20082000)
 *     are owned by Core 0's Arduino-Pico for its task stacks. Core 1 keeps
 *     its stack inside the 64 KB RAM region (top-of-RAM = __StackTop).
 *   - .embedded_block / .embedded_end_block are DISCARDed so the image does
 *     NOT carry an IMAGE_DEF header. Core 0 launches this image via
 *     multicore_launch_core1_raw(), which reads SP/PC straight out of the
 *     first two flash words — any IMAGE_DEF magic at word[0] would replace
 *     the SP value and immediately fault.
 *   - .shared_ipc NOLOAD region is kept identical to Core 0's patched
 *     memmap (0x2007A000..0x20080000, 24 KB) so nm on both ELFs must report
 *     matching g_ipc_shared addresses — this is the sole cross-core coherency
 *     constraint.
 *
 *   - __StackTop = 0x20050000 (top of the 64 KB carve-out)
 *   - SDK runtime places its own .heap/.bss up to __StackLimit; FreeRTOS Heap4
 *     is allocated out of the .bss region (ucHeap[configTOTAL_HEAP_SIZE]).
 *
 * Defines the following symbols for use by code (required by the SDK crt0
 * and pico_runtime_init) — same names/contract as memmap_default.ld:
 *   __exidx_start, __exidx_end
 *   __etext, __data_start__, __data_end__
 *   __preinit_array_start, __preinit_array_end
 *   __init_array_start,    __init_array_end
 *   __fini_array_start,    __fini_array_end
 *   __bss_start__, __bss_end__
 *   __end__, end
 *   __HeapLimit, __StackLimit, __StackTop, __stack
 *   __binary_info_start, __binary_info_end
 *   __default_isrs_start, __default_isrs_end  (used by hardware_exception)
 */

MEMORY
{
    FLASH      (rx) : ORIGIN = 0x10200000, LENGTH = 2048K
    RAM        (rwx): ORIGIN = 0x20040000, LENGTH = 64K
    SHARED_IPC (rw) : ORIGIN = 0x2007A000, LENGTH = 24K
}

/* The SDK's _entry_point is the ELF entry; like the SDK default memmap we
 * pick that symbol (crt0.S declares _reset_handler as local, not global).
 * Note multicore_launch_core1_raw() does not consult ENTRY — it reads SP/PC
 * straight from the first two flash words, so this only affects the ELF
 * header. */
ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        __flash_binary_start = .;
    } > FLASH

    /* First bytes in flash MUST be the Cortex-M vector table:
     *   word[0] = initial SP  (= __StackTop)
     *   word[1] = reset handler (= mokya_core1_reset_handler, Thumb bit set)
     * Everything else in .text follows.
     *
     * Our custom core1_reset.S vector table MUST come before the SDK's
     * crt0.S vector table. The crt0.S _reset_handler does a CPUID check
     * that bounces Core 1 back to the bootrom — we don't want that to win
     * slot 0 of the image. The explicit file-name match below pulls our
     * .vectors first; the generic KEEP picks up crt0.S's (which then lives
     * harmlessly somewhere later in .text but is never dispatched to). */
    .text : {
        __logical_binary_start = .;
        KEEP (*core1_reset.S.obj(.vectors))
        KEEP (*(.vectors))
        /* NOTE: no KEEP(.binary_info_header) and no KEEP(.embedded_block) —
         * see header comment. */
        KEEP (*(.reset))
        *(.init)
        *libgcc.a:cmse_nonsecure_call.o
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
        *(.fini)
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        . = ALIGN(4);
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH

    .rodata : {
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
        *(.srodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx : {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;

    /* Binary info is disabled via PICO_NO_BINARY_INFO=1, but the symbols must
     * still be defined — pico_runtime_init references them unconditionally. */
    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info : {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH
    __binary_info_end = .;
    . = ALIGN(4);

    /* RAM sections — begin at ORIGIN(RAM) = 0x20040000 */

    .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    .data : {
        __data_start__ = .;
        *(vtable)
        *(.time_critical*)
        /* Per the SDK memmap, .text and .rodata that were excluded from FLASH
         * (newlib memcpy/memset/float) land here in RAM so they run at full
         * speed without XIP stalls. */
        *(.text*)
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);
        *(.jcr)
        . = ALIGN(4);
    } > RAM AT> FLASH

    .tdata : {
        . = ALIGN(4);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        __tdata_end = .;
    } > RAM AT> FLASH
    PROVIDE(__data_end__ = .);

    __etext = LOADADDR(.data);

    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        __tls_base = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
        __tls_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __tbss_end = .;
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        *(.sbss*)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD) : {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM
    __HeapLimit = ORIGIN(RAM) + LENGTH(RAM);

    /* Shared inter-core SRAM region. NOLOAD: the ELF carries no bytes for it;
     * Core 0 zero-initialises it via ipc_shared_init() before launching this
     * image. crt0 must NOT clear it (and doesn't — it's outside our RAM
     * region). Core 0's patched memmap_default.ld defines the matching
     * section at the identical address; nm must agree. */
    .shared_ipc (NOLOAD) : ALIGN(4) {
        __shared_ipc_start = .;
        KEEP(*(.shared_ipc))
        __shared_ipc_end = .;
    } > SHARED_IPC

    /* Scratch X/Y regions live inside 0x20080000..0x20082000 and are owned by
     * Core 0. We don't place anything there, but the SDK crt0 data-copy table
     * references __scratch_[xy]_{source,start,end}__ unconditionally, so we
     * must still PROVIDE them. Making start == end makes the copy loop a
     * zero-iteration no-op. Addresses are kept inside our own region so the
     * symbols are semantically meaningful but never read from.
     */
    .scratch_x_stub (NOLOAD) : {
        __scratch_x_start__ = .;
        __scratch_x_end__   = .;
    } > RAM
    __scratch_x_source__ = __scratch_x_start__;

    .scratch_y_stub (NOLOAD) : {
        __scratch_y_start__ = .;
        __scratch_y_end__   = .;
    } > RAM
    __scratch_y_source__ = __scratch_y_start__;

    .flash_end : {
        PROVIDE(__flash_binary_end = .);
    } > FLASH =0xaa

    /* Stack lives at the top of the 64 KB RAM carve. No core 1 stack region
     * because there's only one core in this image. */
    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);
    __StackTop   = ORIGIN(RAM) + LENGTH(RAM);
    __StackBottom = __StackTop - 0x2000;  /* 8 KB reserved for main stack */
    __StackOneTop    = __StackTop;         /* unused in single-core build */
    __StackOneBottom = __StackTop;
    PROVIDE(__stack = __StackTop);

    /* hardware_exception uses these to decide whether an installed handler
     * is still the compile-time default. The crt0 default-ISR stubs live in
     * the .text section; we provide start/end markers so the check never
     * matches (all our handlers are runtime-installed via FreeRTOS/TinyUSB,
     * so "is compile-time default" is always false by construction). */
    PROVIDE(__default_isrs_start = 0);
    PROVIDE(__default_isrs_end   = 0);

    /* picolibc compatibility symbols */
    PROVIDE (__heap_start = __end__);
    PROVIDE (__heap_end = __HeapLimit);
    PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
    PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1));
    PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );
    PROVIDE (_end = __end__);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);

    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")

    /DISCARD/ : {
        /* Drop the bootrom IMAGE_DEF block — see header comment. */
        *(.embedded_block)
        *(.embedded_end_block*)
        *(.boot2)
        *(.binary_info_header)
        *(.note.*)
        *(.comment)
    }
}
