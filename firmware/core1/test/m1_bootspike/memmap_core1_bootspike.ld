/* MokyaLora Phase 2 M1.0b — Core 1 bootspike linker script
 *
 * Minimal Cortex-M33 layout for the dual-image boot spike.
 * Core 0 has already configured XIP; this image lives in the upper half of
 * the 16 MB QSPI flash at 0x10200000 and is launched via
 * multicore_launch_core1_raw() from Core 0's initVariant().
 *
 * No RP2350 bootrom IMAGE_DEF metadata is emitted — Core 1 is not booted by
 * the bootrom from this image, so the usual IMAGE_DEF block is unnecessary.
 * The first word of FLASH must be the initial SP and the second word the
 * reset-handler address (Thumb bit set) per the Cortex-M exception model;
 * Core 0 passes them through the FIFO handshake as sp and entry.
 *
 * SRAM carve-out at 0x20040000..0x20050000 (64 KB) is chosen to stay above
 * Core 0's .data/.bss (which sit at the low end of SRAM, ~54 KB at M1.0)
 * and below the .shared_ipc reservation at 0x2007A000..0x20082000 planned
 * for M1.1. The sentinel at 0x20078000 is written from main() and read back
 * by the host over SWD after Core 0 boots.
 */

MEMORY
{
    FLASH (rx) : ORIGIN = 0x10200000, LENGTH = 64K
    RAM   (rw) : ORIGIN = 0x20040000, LENGTH = 64K
}

ENTRY(reset_handler)

SECTIONS
{
    .vectors ORIGIN(FLASH) : ALIGN(128)
    {
        __vector_table__ = .;
        KEEP(*(.vectors))
    } > FLASH

    .text : ALIGN(4)
    {
        *(.text*)
        *(.rodata*)
    } > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        __bss_end__ = ALIGN(4);
    } > RAM

    /* Stack grows down from the top of RAM. */
    __stack_top__ = ORIGIN(RAM) + LENGTH(RAM);

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.note.*)
        *(.comment)
    }
}
