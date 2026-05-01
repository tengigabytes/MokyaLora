/* lfs_user_config.h — User-supplied configuration for vendored LittleFS.
 *
 * Pulled in by lfs_util.h via `#ifdef LFS_CONFIG / #include LFS_CONFIG`.
 * Set on the compile line as `-DLFS_CONFIG=\"lfs_user_config.h\"` (the
 * include path lets LFS source resolve the header without modification).
 *
 * Job: redirect lfs_malloc/lfs_free to a FreeRTOS heap-backed wrapper
 * (firmware/core1/src/storage/lfs_blockdev.c::c1_lfs_alloc/dealloc).
 * Without this, vendored lfs_util.h's inline lfs_malloc calls libc's
 * malloc which Pico SDK wraps to pico_malloc — and pico_malloc panics
 * on NULL when the tiny ~1.4 KB newlib heap fills up (which happens
 * on the very first lfs_file_opencfg's 256 B file-cache allocation).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void *c1_lfs_alloc(size_t size);
extern void  c1_lfs_dealloc(void *p);

#ifdef __cplusplus
}
#endif

#define LFS_MALLOC  c1_lfs_alloc
#define LFS_FREE    c1_lfs_dealloc
