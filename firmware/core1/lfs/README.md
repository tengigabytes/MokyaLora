# LittleFS — vendored for Core 1

Direct copy of LittleFS v2.11 (`LFS_VERSION = 0x0002000b`) from
Arduino-Pico's `framework-arduinopico/libraries/LittleFS/lib/littlefs/`,
which is itself vendored from upstream
[`littlefs-project/littlefs`](https://github.com/littlefs-project/littlefs).

License: **BSD-3-Clause** (`LICENSE.md`). Compatible with Core 1's
Apache-2.0 image license.

## Why vendor (not submodule)

- Avoids the Meshtastic-style submodule auth round-trips for routine
  edits.
- Core 1 already isolates Pico SDK + FreeRTOS + LVGL as direct sources
  (no PIO / Arduino-Pico package layer).
- Upstream LittleFS API is highly stable across v2.x; the cost of
  manual periodic re-sync is negligible.

## Files

| File | Purpose |
|---|---|
| `lfs.c` / `lfs.h` | Filesystem core |
| `lfs_util.c` / `lfs_util.h` | Util macros (logging, asserts, memcpy stubs) |
| `LICENSE.md` | BSD-3-Clause text |

## Local edits

- `lfs_util.h`: a 5-line forward declaration of `c1_lfs_alloc` /
  `c1_lfs_dealloc` (gated on `LFS_MALLOC || LFS_FREE`) was inserted
  immediately above the `lfs_malloc` inline. Required because we
  redirect LFS allocations to the FreeRTOS heap (52 KB) — Pico SDK's
  newlib heap is only ~1.4 KB and `pico_malloc` panics on NULL.
  Re-apply on every LittleFS re-vendor.

## Update procedure

1. Pull latest stable release from upstream (or whichever Arduino-Pico
   ships).
2. Copy `lfs.{c,h}` + `lfs_util.{c,h}` over.
3. Re-run `c1_storage_self_test` on Rev A — must PASS before commit.
4. Bump version note in this README.
