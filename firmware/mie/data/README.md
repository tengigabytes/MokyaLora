# MIE data — generated binary assets

This directory holds compiled binaries that the build/flash flow consumes.
None of these files are intended to be hand-edited; regenerate via the
tools in `firmware/mie/tools/`.

## Active assets (default flash flow)

| File | Format | Generator | Consumer |
|------|--------|-----------|----------|
| `dict_mie_v4.bin`        | MIE4 v4 (single blob, magic `MIE4` / `0x3445494D`) | `gen_dict.py --v4-output` | `bash scripts/build_and_flash.sh` flashes to `0x10400000` |
| `mie_unifont_sm_16.bin`  | MIEF v1 font           | `gen_font.py`            | `bash scripts/build_and_flash.sh` flashes to `0x10A00000` |

`dict_mie_v4.bin` is the only dict the device should ever boot with.
Core 1's `mie_dict_loader.c` auto-detects via the 4-byte magic and the
host-side `scripts/ime_text_test.py` actively rejects anything else.

## Retired assets — kept for archaeology only (2026-04-26, P3-5)

| File | Status | Notes |
|------|--------|-------|
| `dict_dat.bin`      | RETIRED | MDBL v2 trie data (zh_dat).   Do not flash. |
| `dict_values.bin`   | RETIRED | MDBL v2 candidate values.     Do not flash. |
| `en_dat.bin`        | RETIRED | MDBL v2 SmartEn trie data.    v4 embeds English inside the MIE4 blob. |
| `en_values.bin`     | RETIRED | MDBL v2 SmartEn values.       v4 embeds English inside the MIE4 blob. |

These four files together formed the legacy MDBL v2 pack consumed by
the `mie_dict_blob` CMake target in `build/mie-host/`. The whole format
was retired on 2026-04-26 because:

- v2 ranks differently from v4, so any benchmark on v2 reads ~4× slower
  than the current production baseline (~430 ms/char with `--user-sim`,
  v2 hits ~1700 ms/char).
- The personalised LRU cache (Phase 1.6) and slot-key UX changes only
  apply to the v4 search path.
- Coverage on v2 was capped at 1,479 single-syllable Bopomofo readings;
  v4 has ~3× that under composition lookup.

If you absolutely need to flash MDBL (e.g. bisecting a v4 regression
against the v2 baseline), use `bash scripts/build_and_flash.sh
--v2-deprecated --dict`. The script will print a loud warning and pause
3 s before flashing. Do not commit changes that re-enable v2 by default.

The v2 generator path in `firmware/mie/tools/gen_dict.py` (`--output-dir`
without `--v4-output`) is also kept as archaeology — it still produces
valid MDBL but is not exercised by the CI / default build flow.
