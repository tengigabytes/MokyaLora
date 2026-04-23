"""mokya_rtt.py — pylink RTT transport for Mokya virtual key injection.

Mirrors the SWD-RAM ring protocol in mokya_swd.py but pushes frames down
the SEGGER RTT down-channel MOKYA_RTT_KEYINJ_CHAN (channel 1). Firmware
side: firmware/core1/src/keypad/key_inject_rtt.c.

Typical use:

    with MokyaRtt() as rtt:
        rtt.send_key(0x19, pressed=True)          # MODE press
        rtt.send_key(0x19, pressed=False)
        rtt.send_force_save()                     # convenience: flush LRU

Wire frame (shared with key_inject_frame.h):

    0xAA 0x55 | type | len | payload... | crc8
    type = 0x01 key event   (payload: key_byte, flags_byte)
    type = 0x02 force save  (payload: empty)
    type = 0x03 nop         (payload: empty — keepalive)

SPDX-License-Identifier: MIT
"""
import subprocess
import time
from pathlib import Path

import pylink

DEFAULT_ELF = "build/core1_bridge/core1_bridge.elf"
ARM_NM = (r"C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/"
          r"bin/arm-none-eabi-nm.exe")

MAGIC0 = 0xAA
MAGIC1 = 0x55

TYPE_KEY_EVENT  = 0x01
TYPE_FORCE_SAVE = 0x02
TYPE_NOP        = 0x03

KEYINJ_CHAN = 1

# Match SDK default — RTT control block lives inside the first 512 KB of
# SRAM, which on RP2350 starts at 0x20000000. Leaving this explicit avoids
# the "control block not found" warning that fires when pylink scans all
# of memory on its own.
RTT_SEARCH_START = 0x20000000
RTT_SEARCH_LEN   = 0x80000


def crc8(data: bytes) -> int:
    """CRC-8/ITU (poly 0x07, init 0x00). Mirrors mokya_kij_crc8 in the
    firmware header so frames round-trip byte-identically."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def build_frame(type_: int, payload: bytes = b"") -> bytes:
    if len(payload) > 16:
        raise ValueError("payload > MOKYA_KIJ_MAX_PAYLOAD")
    body = bytes([type_, len(payload)]) + payload
    return bytes([MAGIC0, MAGIC1]) + body + bytes([crc8(body)])


class MokyaRtt:
    """Thin wrapper over pylink.JLink for the RTT key-inject channel.

    Bypasses pylink.rtt_write() and writes the SEGGER RTT down buffer
    directly via memory_write8. Rationale: on RP2350 + J-Link V932,
    pylink.rtt_write returns success but subsequent writes within a
    session silently fail to advance WrOff — likely a race between
    pylink's buffered client and the J-Link background DAP thread.
    Going straight through the SEGGER control block is what SEGGER
    RTT's own client does and is deterministic.

    The connection is persistent over the context manager; firmware
    side uses NO_BLOCK_SKIP so if the ring fills up new writes return
    short rather than block the host. Keep `send_*` rate below ~100
    frames/s to stay under the 256 B down-buffer headroom."""

    # SEGGER RTT control block layout (see SEGGER_RTT.h):
    #   +0   char acID[16] = "SEGGER RTT\0..."
    #   +16  u32 MaxNumUpBuffers
    #   +20  u32 MaxNumDownBuffers
    #   +24  UP_BUFFER aUp[MaxNumUpBuffers]
    #   +...  DOWN_BUFFER aDown[MaxNumDownBuffers]
    # Each buffer descriptor is 24 bytes:
    #   +0  name*  +4 buf*  +8 size  +12 WrOff  +16 RdOff  +20 Flags
    _HDR = 24
    _DESC = 24

    def __init__(self, device="RP2350_M33_1", speed=4000, elf=DEFAULT_ELF):
        self.device = device
        self.speed  = speed
        self.elf    = elf
        self._jl    = None
        self._buf_ptr = None     # down[KEYINJ_CHAN].buf pointer
        self._buf_size = None
        self._wroff_addr = None  # address of down[KEYINJ_CHAN].WrOff
        self._rdoff_addr = None

    def _find_rtt_cb(self):
        """Look up _SEGGER_RTT in the ELF so pylink.rtt_start can be
        given an explicit block address. Without it, pylink's auto-
        search window is too narrow on RP2350 and just returns
        'RTT Control Block has not yet been found' indefinitely."""
        r = subprocess.run([ARM_NM, self.elf], capture_output=True,
                           text=True, check=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == "_SEGGER_RTT":
                return int(parts[0], 16)
        raise RuntimeError("_SEGGER_RTT symbol not found in "
                           f"{self.elf}")

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *a):
        self.close()

    def open(self):
        if self._jl is not None: return
        jl = pylink.JLink()
        jl.open()
        jl.set_tif(pylink.enums.JLinkInterfaces.SWD)
        jl.connect(self.device, speed=self.speed)
        if not jl.target_connected():
            jl.close()
            raise RuntimeError(f"failed to connect to {self.device}")
        # Locate the RTT control block from the ELF, read the key-inject
        # down descriptor out of target memory, and cache pointers so
        # send_frame() can go straight to memory_write without any
        # rtt_start / rtt_write round-trips.
        cb = self._find_rtt_cb()
        max_up = jl.memory_read32(cb + 16, 1)[0]
        desc_off = cb + self._HDR + max_up * self._DESC \
                   + KEYINJ_CHAN * self._DESC
        d = jl.memory_read32(desc_off, 6)
        name_ptr, buf_ptr, size, wroff, rdoff, flags = d
        if size == 0 or buf_ptr == 0:
            jl.close()
            raise RuntimeError(
                f"RTT down-buffer {KEYINJ_CHAN} not configured — firmware "
                "has not called SEGGER_RTT_ConfigDownBuffer yet.")
        self._buf_ptr     = buf_ptr
        self._buf_size    = size
        self._wroff_addr  = desc_off + 12
        self._rdoff_addr  = desc_off + 16
        self._jl = jl

    def close(self):
        if self._jl is not None:
            try: self._jl.close()
            except Exception: pass
            self._jl = None

    # ── Direct RTT ring writer ────────────────────────────────────────

    def _free_space(self, wr, rd):
        # SEGGER leaves one byte unused so full/empty are distinguishable.
        return (rd - wr - 1) % self._buf_size

    def send_frame(self, frame: bytes):
        """Write bytes into the down-ring at [WrOff..], wrap, then
        publish new WrOff. Mirrors the target side of SEGGER_RTT_Read
        so firmware's SEGGER_RTT_HasData() sees the bytes immediately.
        Raises if the ring lacks room (caller should pace)."""
        n = len(frame)
        if n == 0: return 0
        wr = self._jl.memory_read32(self._wroff_addr, 1)[0]
        rd = self._jl.memory_read32(self._rdoff_addr, 1)[0]
        if self._free_space(wr, rd) < n:
            raise RuntimeError(
                f"RTT down-ring full: free={self._free_space(wr, rd)}, "
                f"want={n} (firmware stalled?)")
        # Write — split around the wrap if needed.
        end = wr + n
        if end <= self._buf_size:
            self._jl.memory_write8(self._buf_ptr + wr, list(frame))
        else:
            first = self._buf_size - wr
            self._jl.memory_write8(self._buf_ptr + wr, list(frame[:first]))
            self._jl.memory_write8(self._buf_ptr, list(frame[first:]))
        # Publish new WrOff last (acts as a release barrier from the
        # target's point of view).
        self._jl.memory_write32(self._wroff_addr, [end % self._buf_size])
        return n

    def send_key(self, keycode: int, pressed: bool, flags: int = 0):
        key_byte = (keycode & 0x7F) | (0x80 if pressed else 0x00)
        self.send_frame(build_frame(TYPE_KEY_EVENT,
                                    bytes([key_byte, flags & 0xFF])))

    def send_force_save(self):
        self.send_frame(build_frame(TYPE_FORCE_SAVE))

    def send_nop(self):
        self.send_frame(build_frame(TYPE_NOP))


def _selftest():
    """Minimal end-to-end smoke test. Requires the board to be running
    firmware with key_inject_rtt_task live. Injects a MODE press/release
    and a force-save frame, and prints parser stats read via SWD."""
    import sys
    sys.path.insert(0, str(__import__("pathlib").Path(__file__).parent))
    from mokya_swd import MokyaSwd  # type: ignore

    with MokyaRtt() as rtt:
        print("[selftest] sending 1x MODE press/release + 1x NOP")
        rtt.send_key(0x19, pressed=True)
        rtt.send_key(0x19, pressed=False)
        rtt.send_nop()
        time.sleep(0.5)

    with MokyaSwd(device="RP2350_M33_1") as swd:
        # Parser stats are file-scope statics — read via symbol lookup.
        for name in ("s_rtt_bytes_read", "s_rtt_frames_ok", "s_rtt_rejected"):
            try:
                addr = swd.symbol(name)
                val  = swd.read_u32(addr)
                print(f"[selftest] {name:24} = {val}")
            except Exception as e:
                print(f"[selftest] {name}: {e}")


if __name__ == "__main__":
    _selftest()
