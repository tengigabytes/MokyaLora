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
