/* MokyaLora Phase 2 M1.1-A — Core 1 bridge linker script
 *
 * Same boot model as the M1.0b bootspike (this image is launched by Core 0
 * via multicore_launch_core1_raw(), NOT by the RP2350 bootrom, so there is no
 * IMAGE_DEF block and the first two flash words are the initial SP and reset
 * vector read by the Cortex-M exception model — Core 0 reads them out of this
 * image and forwards them through the FIFO handshake).
 *
 * New vs bootspike:
 *   - .data / COMMON fully wired up (bridge code has static state)
 *   - SHARED_IPC memory region at 0x2007A000 (24 KB, NOLOAD) — must match
 *     Core 0's patched memmap_default.ld so nm reports identical
 *     g_ipc_shared addresses on both sides.
 *
 * Memory map recap (RP2350B, 512 KB main SRAM):
 *   0x20000000  Core 0 .data / .bss / heap     (Arduino-Pico managed)
 *   0x20040000  Core 1 RAM carve-out           (this image)
 *   0x20050000  Core 1 stack top               (64 KB carve-out ends)
 *   0x20078000  M1.0b sentinel + debug breadcrumbs (preserved)
 *   0x2007A000  Shared IPC region (24 KB)      (this image + Core 0)
 *   0x20080000  SCRATCH_X / SCRATCH_Y          (Core 0 FreeRTOS task stacks)
 */

MEMORY
{
    FLASH      (rx) : ORIGIN = 0x10200000, LENGTH = 128K
    RAM        (rw) : ORIGIN = 0x20040000, LENGTH = 64K
    SHARED_IPC (rw) : ORIGIN = 0x2007A000, LENGTH = 24K
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

    /* Initialised data: LMA in FLASH, VMA in RAM. crt0 copies at reset. */
    .data : ALIGN(4)
    {
        __data_start__ = .;
        *(.data*)
        __data_end__ = ALIGN(4);
    } > RAM AT > FLASH
    __data_load__ = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        __bss_end__ = ALIGN(4);
    } > RAM

    /* Shared inter-core SRAM region. NOLOAD: the ELF carries no bytes for it,
     * the linker only assigns addresses. Core 0 explicitly zero-initialises it
     * before launching this image; crt0 must NOT clear it. */
    .shared_ipc (NOLOAD) : ALIGN(4)
    {
        __shared_ipc_start = .;
        KEEP(*(.shared_ipc))
        __shared_ipc_end = .;
    } > SHARED_IPC

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
