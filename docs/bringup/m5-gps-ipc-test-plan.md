# M5 GPS IPC Bridge — Hardware Test Plan

**Milestone:** M5 GPS time + position propagation Core 1 → Core 0  
**Date:** 2026-05-02  
**Branch:** `dev-Sblzm`

---

## Summary

Core 1 `gps_task` now writes structured `IpcGpsFixSlot` snapshots into the
shared-SRAM `IpcGpsBuf` double-buffer.  Core 0 `IpcGpsConsumer` (1 Hz
OSThread) reads the most-recently-published slot and calls:
- `perhapsSetRTC(RTCQualityGPS, &tv)` — disciplines the Meshtastic wall clock
- `nodeDB->setLocalPosition(pos)` — triggers `PositionModule::runOnce()` to
  broadcast position to the mesh

The real Teseo driver path is gated behind `#ifndef MOKYA_GPS_DUMMY_NMEA`.
For initial hardware testing, use the dummy injector
(`-DMOKYA_GPS_DUMMY_NMEA=ON` in Core 1's CMake config); it fires a
`IpcGpsFixSlot` at 1 Hz with fixed Taipei coordinates
(25.052103 N / 121.574039 E) and a monotonically-incrementing Unix epoch
anchored to 2026-04-26 00:00:00 UTC.

---

## Prerequisites

1. Dual-image firmware flashed via J-Link:
   ```sh
   bash scripts/build_and_flash.sh
   ```
   or independently:
   ```sh
   # Core 0 only
   python -m platformio run -e rp2350b-mokya -d firmware/core0/meshtastic
   # Core 1 only (with dummy NMEA enabled)
   cmake -DMOKYA_GPS_DUMMY_NMEA=ON ...
   bash scripts/build_and_flash.sh --core1
   ```
2. J-Link Ultra V6 connected via SWD.
3. Host machine with `python -m meshtastic` (v2.7.8) available.

---

## Test 1 — IpcGpsBuf sanity via SWD (no serial required)

Verify that Core 1's dummy injector is writing structured fix slots into
shared SRAM at `g_ipc_shared.gps_buf`.

### Procedure

1. Power-cycle or reset the board.
2. Wait ≥ 2 seconds for the GPS dummy task to have written at least two slots.
3. Open a J-Link Commander session (device `RP2350_M33_0`, speed 4000):
   ```
   connect
   h
   ```
4. Read `write_idx` (first byte of `IpcGpsBuf._pad` is at +48+1 = offset 49
   from the start of `gps_buf`; but `write_idx` is at offset 48 of the struct):
   ```
   ; IpcSharedSram base = 0x2007A000
   ; gps_buf offset = boot_handshake(28) + wd_pause(4) + ring_ctrl(3×32)
   ;                + ring_slots(3×80×264) + _tail_pad_pre(1788)
   ; = 28+4+96+63360+1788 = 65276 = 0xFF3C
   ; gps_buf start = 0x2007A000 + 0xFF3C = 0x2007FF3C  (verify via map file)
   mem8 <gps_buf_addr> 1          ; read write_idx byte
   ```
   The address can be pinned from the map:
   ```sh
   grep "g_ipc_shared" firmware/core0/meshtastic/.pio/build/rp2350b-mokya/firmware*.map
   ```
5. Read 8 bytes of `slot[write_idx ^ 1]` (the published slot):
   ```
   mem32 <gps_buf_addr+slot_offset> 4
   ```
   Expected (`slot[0]` if `write_idx=1`, `slot[1]` if `write_idx=0`):
   - `unix_epoch` ≈ `0x67EC9A00` + seconds-since-boot (little-endian u32)
   - `lat_e7` = `0x0EED81C6` (250521030 = 25.052103 × 1e7)
   - `lon_e7` = `0x486AFC46` (1215740390 = 121.574039 × 1e7)
   - `altitude_mm` = `0x0000C350` (50000 = 50 m)

### Pass criteria
- `write_idx` is 0 or 1 (not 255 = uninitialised).
- `unix_epoch` ≠ 0.
- `lat_e7` and `lon_e7` match the expected constants above.
- Repeated reads (≥ 3 s apart) show `unix_epoch` incrementing by ~3
  (one per second, three seconds elapsed).

---

## Test 2 — `meshtastic --info` reports GPS coordinates

Verify that Core 0 `IpcGpsConsumer` is propagating the fix to `nodeDB` and
that the Meshtastic CLI reflects the position.

### Procedure

1. Flash both cores with `bash scripts/build_and_flash.sh`.
2. Wait ≥ 5 s for USB CDC to enumerate and the position thread to fire at
   least once.
3. Run:
   ```sh
   python -m meshtastic --port <COMxx> --info
   ```
4. Inspect the `my_info` / `nodes` section for the local node.

### Pass criteria
- `locationSource` = `LOC_INTERNAL` (not `LOC_MANUAL` or absent).
- `latitude` ≈ 25.052103 (±0.001°).
- `longitude` ≈ 121.574039 (±0.001°).
- `altitude` ≈ 50 m.
- `time` field (Unix epoch) matches approximately `1745625600 + uptime_s`.

---

## Test 3 — RTC disciplines from GPS epoch

Verify that `perhapsSetRTC(RTCQualityGPS, ...)` has run and the modem's
wall clock reflects the GPS epoch.

### Procedure

1. After Test 2 passes, run:
   ```sh
   python -m meshtastic --port <COMxx> --info
   ```
2. Check `myInfo.reboot_count` or any field with a `time` component.
3. Alternatively, inspect the Meshtastic debug log line beginning with
   `Set RTC quality=GPS` (visible over USB CDC at baud 115200):
   ```sh
   python -m meshtastic --port <COMxx> --listen
   ```
   Expected log fragment:
   ```
   Set RTC quality=GPS to <unix_epoch>
   ```

### Pass criteria
- The RTC log line appears within 5 s of boot with `quality=GPS`.
- The Unix epoch in the log matches the dummy injector's `DUMMY_UNIX_BASE +
  uptime_s` to within ±2 s.

---

## Test 4 — PositionModule broadcasts to mesh

Verify that the local node transmits its position to mesh neighbours.

### Procedure

1. Have a second Meshtastic node within RF range.
2. Flash Core 0 + Core 1 on the DUT (MokyaLora).
3. On the second node, run:
   ```sh
   python -m meshtastic --port <second_node_port> --info
   ```
4. Look for the DUT node in the `nodes` list on the second node.

### Pass criteria
- The DUT's node entry on the second node includes position data matching
  the dummy coordinates.
- `locationSource` = `LOC_INTERNAL` on the remote node's copy of the DUT
  node info.

---

## Test 5 — Real Teseo integration (deferred, `MOKYA_GPS_DUMMY_NMEA=OFF`)

Once outdoor testing is available:

1. Build with `MOKYA_GPS_DUMMY_NMEA=OFF` (default).
2. Bring the board outdoors, wait for GNSS cold-start fix (≤ 90 s typical).
3. Run `python -m meshtastic --info` and verify:
   - `latitude` / `longitude` match the actual location (±50 m HDOP circle).
   - `time` matches real UTC to within ±2 s.
   - `sats_in_view` ≥ 4.

---

## SWD IpcGpsBuf address derivation

If the map file lookup is preferred over manual arithmetic:

```sh
# From repo root, after a successful Core 0 build:
MAP=firmware/core0/meshtastic/.pio/build/rp2350b-mokya/firmware.elf
arm-none-eabi-nm -n "$MAP" | grep g_ipc_shared
```

`g_ipc_shared` is the singleton `IpcSharedSram` placed in `.shared_ipc_sram`
(linker section `NOLOAD` at `0x2007A000`).  The `gps_buf` field offset within
`IpcSharedSram` can be read from the struct layout (see `ipc_shared_layout.h`
`_Static_assert` guards).

---

## Known limitations

- The dummy injector produces a static position; actual position tracking
  requires Test 5 with real Teseo output.
- `PositionModule::runOnce()` broadcasts on its own interval (default
  `position_broadcast_secs` = 900 s); it can be forced with
  `positionModule->sendOurPosition()` over the Meshtastic admin interface.
- `IpcGpsStream` is now a permanent no-op; any NMEA-based GPS probe path is
  fully disabled.  GPS module state is set to `connected` by the
  `MOKYA_IPC_GPS_STREAM` init hook in variant.cpp; this is required to
  keep `PositionModule` from disabling itself.
