"""mokya_swd.py — persistent pylink-based SWD helper for Mokya Core 1.

Replaces the J-Link Commander subprocess calls used by the initial
inject_keys.py prototype. Connection is opened once per Python run and
every mem read / write is ~0.3 ms instead of 500 ms per command.

Typical use:
    with MokyaSwd() as swd:
        magic = swd.read_u32(0x20066364)
        swd.write_u8_many([(0x20066390, 0x8E), (0x20066391, 0x0E)])
"""
import subprocess
from pathlib import Path

import pylink

DEFAULT_ELF    = "build/core1_bridge/core1_bridge.elf"
DEFAULT_DEVICE = "RP2350_M33_1"
ARM_NM = (r"C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/"
          r"bin/arm-none-eabi-nm.exe")


class MokyaSwd:
    """Thin wrapper over pylink.JLink. Use as context manager."""

    def __init__(self, device=DEFAULT_DEVICE, speed=4000,
                 elf=DEFAULT_ELF):
        self.device = device
        self.speed  = speed
        self.elf    = elf
        self._jl    = None
        self._sym_cache = {}
        # RTT down-buffer cache (populated on first rtt_send_frame).
        self._rtt_buf_ptr   = None
        self._rtt_buf_size  = None
        self._rtt_wroff_addr = None
        self._rtt_rdoff_addr = None

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
        self._jl = jl

    def close(self):
        if self._jl is not None:
            try: self._jl.close()
            except Exception: pass
            self._jl = None

    # ── Memory I/O (all non-halting; target keeps running) ────────────

    def read_u32(self, addr):
        last = None
        for attempt in range(4):
            try: return self._jl.memory_read32(addr, 1)[0]
            except Exception as e:
                last = e; import time; time.sleep(0.05 * (attempt + 1))
        raise last

    def read_u32_many(self, addr, count):
        last = None
        for attempt in range(4):
            try: return self._jl.memory_read32(addr, count)
            except Exception as e:
                last = e; import time; time.sleep(0.05 * (attempt + 1))
        raise last

    def read_mem(self, addr, nbytes):
        """pylink occasionally returns a JLinkReadException on long runs
        (USB hiccup, SWD rate-limit). Retry the read a few times before
        bubbling the error up — almost all transient failures clear
        within one retry."""
        last = None
        for attempt in range(4):
            try:
                return bytes(self._jl.memory_read8(addr, nbytes))
            except Exception as e:
                last = e
                import time
                time.sleep(0.05 * (attempt + 1))
        raise last

    def write_u32(self, addr, value):
        self._jl.memory_write32(addr, [value & 0xFFFFFFFF])

    def write_u8_many(self, ops):
        """ops: list of (addr, byte). Fast-path contiguous writes."""
        if not ops: return
        # Group contiguous runs.
        ops = sorted(ops)
        run_start = ops[0][0]
        run_data  = [ops[0][1] & 0xFF]
        for addr, byte in ops[1:]:
            if addr == run_start + len(run_data):
                run_data.append(byte & 0xFF)
            else:
                self._jl.memory_write8(run_start, run_data)
                run_start, run_data = addr, [byte & 0xFF]
        self._jl.memory_write8(run_start, run_data)

    # ── RTT down-channel direct write ─────────────────────────────────
    # Writes bytes straight into the SEGGER RTT down-buffer registered
    # by firmware/core1/src/keypad/key_inject_rtt.c. Bypasses pylink's
    # rtt_write(), which on RP2350 + J-Link V932 silently drops all but
    # the first write per session. Safe to mix with normal SWD reads.

    _RTT_HDR  = 24   # SEGGER CB: 16 B id + 4 B max_up + 4 B max_down
    _RTT_DESC = 24   # per-buffer descriptor size
    _KEYINJ_CHAN = 1

    def _rtt_locate(self):
        if self._rtt_buf_ptr is not None: return
        cb = self.symbol("_SEGGER_RTT")
        max_up = self._jl.memory_read32(cb + 16, 1)[0]
        d_off = cb + self._RTT_HDR + max_up * self._RTT_DESC \
                + self._KEYINJ_CHAN * self._RTT_DESC
        d = self._jl.memory_read32(d_off, 6)
        _, buf_ptr, size, _, _, _ = d
        if size == 0 or buf_ptr == 0:
            raise RuntimeError(
                f"RTT down-buffer {self._KEYINJ_CHAN} not configured — "
                "firmware key_inject_rtt task not started?")
        self._rtt_buf_ptr   = buf_ptr
        self._rtt_buf_size  = size
        self._rtt_wroff_addr = d_off + 12
        self._rtt_rdoff_addr = d_off + 16

    def rtt_send_frame(self, frame, timeout_s=1.0):
        """Write a single framed byte sequence into the RTT down-ring.
        Blocks up to timeout_s for ring space. Raises on timeout."""
        self._rtt_locate()
        n = len(frame)
        if n == 0: return 0
        import time as _t
        deadline = _t.time() + timeout_s
        while True:
            wr = self._jl.memory_read32(self._rtt_wroff_addr, 1)[0]
            rd = self._jl.memory_read32(self._rtt_rdoff_addr, 1)[0]
            free = (rd - wr - 1) % self._rtt_buf_size
            if free >= n: break
            if _t.time() > deadline:
                raise RuntimeError(
                    f"RTT ring full for {timeout_s}s (wr={wr} rd={rd})")
            _t.sleep(0.002)
        end = wr + n
        if end <= self._rtt_buf_size:
            self._jl.memory_write8(self._rtt_buf_ptr + wr, list(frame))
        else:
            first = self._rtt_buf_size - wr
            self._jl.memory_write8(self._rtt_buf_ptr + wr, list(frame[:first]))
            self._jl.memory_write8(self._rtt_buf_ptr, list(frame[first:]))
        self._jl.memory_write32(self._rtt_wroff_addr,
                                [end % self._rtt_buf_size])
        return n

    def rtt_ring_empty(self):
        """True if the firmware has drained everything the host pushed."""
        self._rtt_locate()
        wr = self._jl.memory_read32(self._rtt_wroff_addr, 1)[0]
        rd = self._jl.memory_read32(self._rtt_rdoff_addr, 1)[0]
        return wr == rd

    # ── ELF symbol lookup (one-off, cached) ──────────────────────────

    def symbol(self, name):
        if name in self._sym_cache: return self._sym_cache[name]
        r = subprocess.run([ARM_NM, self.elf], capture_output=True,
                           text=True, check=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == name:
                addr = int(parts[0], 16)
                self._sym_cache[name] = addr
                return addr
        # Try mangled C++ anonymous-namespace names too.
        needle = f"L{len(name)}{name}E"
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and needle in parts[2]:
                addr = int(parts[0], 16)
                self._sym_cache[name] = addr
                return addr
        raise RuntimeError(f"symbol {name!r} not found in {self.elf}")
