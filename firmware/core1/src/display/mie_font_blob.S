/* mie_font_blob.S — embed the MIEF font binary into Core 1's .rodata.
 *
 * The absolute path to the .bin is passed via the MIEF_BLOB_PATH compile
 * definition (a quoted string). CMake's add_custom_command regenerates
 * the .bin from firmware/mie/data/charlist.txt + unifont-17.0.04.otf
 * whenever those inputs change.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

    .section .rodata
    .balign 4
    .global _mie_unifont_sm_16_start
    .global _mie_unifont_sm_16_end
    .global _mie_unifont_sm_16_size
_mie_unifont_sm_16_start:
    .incbin MIEF_BLOB_PATH
_mie_unifont_sm_16_end:
    .balign 4
_mie_unifont_sm_16_size:
    .long _mie_unifont_sm_16_end - _mie_unifont_sm_16_start
