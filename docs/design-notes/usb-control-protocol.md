# USB Control Protocol

**Project:** MokyaLora (Project MS-RP2350)
**Status:** Design — targeting Phase 2 post-M4 milestone
**Last updated:** 2026-04-15

This document specifies the **PC-side Control Protocol** exposed over a dedicated
USB CDC interface (`CDC#1`) for automated testing, remote UI control, and
host-driven input injection. It is the single source of truth for the wire
format, command set, ACK semantics, authentication, and safety gates of that
interface.

For the broader USB mode architecture (composite device, OFF/COMM mode
selection, CDC#0 Meshtastic bridge), see
[`firmware-architecture.md`](firmware-architecture.md) §4.6.
For the requirement-level definition (why this interface exists, how it is
gated, non-functional guarantees), see
[`software-requirements.md`](../requirements/software-requirements.md) §6.
For the `keycode.h` table referenced by `KEY` commands, see
[`mie-architecture.md`](mie-architecture.md) §7.

---

## 1. Purpose

The Control Protocol serves three use cases:

1. **Automated regression testing** — CI drives real hardware via a Python
   harness; each command carries a sequence number and returns a deterministic
   ACK so the test script can synchronise without polling.
2. **Remote debugging** — developer controls a device that is physically out
   of reach (e.g. a prototype left at another location). Key injection and
   framebuffer capture together replace "can you press OK and tell me what's
   on screen?".
3. **UI / IME development velocity** — host-side tooling (`mokya-ctl`,
   delta screenshots, state diffs) without flashing J-Link.

Explicit non-goals:

- Not a user-facing feature. Shipping firmware includes the interface but with
  the runtime gate closed by default.
- Not a Meshtastic transport. Meshtastic CLI remains on CDC#0 via the byte
  bridge (§4.6).
- Not a file-transfer or OTA channel. Image update uses the Pico bootrom
  `reset_usb_boot()` path (FA §10.5).

---

## 2. USB Interface Architecture

Core 1 owns USB (FA §4.6). The Control Protocol lives on a second CDC
interface on the same composite device.

| Interface | Host sees | Owner task | Protocol |
|-----------|-----------|------------|----------|
| CDC#0     | virtual COM (Meshtastic) | `USBTask`    | Meshtastic CLI byte bridge — unchanged |
| CDC#1     | virtual COM (Control)    | `UsbCtrlTask`| Control Protocol (this document)       |

Both interfaces enumerate together in **Mode COMM** (the only TinyUSB-enabled
mode — see FA §4.6). Mode OFF leaves the entire USB device dormant; neither
interface enumerates.

Rationale for composite (two CDC) over interface-switch or protocol multiplex
on a single CDC:

- The two streams have different framing (CDC#0 is transparent bytes driving
  Meshtastic framing; CDC#1 is SLIP+COBS). Multiplexing on one CDC would require
  a meta-framing layer that corrupts Meshtastic's own framing.
- Host-side test harnesses can open CDC#1 without touching CDC#0, and vice
  versa. The two COM ports are independently addressable in `pyserial`.
- TinyUSB composite CDC configuration is stable and well-trod; no custom
  descriptor work beyond declaring a second CDC function.

---

## 3. Framing — SLIP + COBS

Each logical packet is framed as:

```
   ┌──────┬──────────────┬───────────┐
   │ COBS │  PACKET BODY │ 0x00 byte │
   └──────┴──────────────┴───────────┘
```

1. Build the packet body (§4).
2. Apply **COBS** encoding to eliminate all `0x00` bytes inside the body.
3. Append a single `0x00` terminator byte.

A reader accumulates bytes until it sees `0x00`, runs COBS-decode over the
accumulated buffer, and treats the result as the packet body. A stray `0x00`
in the stream only costs one discarded (probably empty) packet — the next
complete frame still parses.

This choice satisfies the `SCREEN` command (binary framebuffer payloads up to
~150 KB) without Base64 bloat, while keeping host-side debugging trivial (a
one-line COBS decoder works on captured serial logs).

Maximum packet body size: **1024 bytes** request / **16384 bytes** response.
`SCREEN` is the only command that approaches the response cap; see §5.5 for
how it fragments.

---

## 4. Packet Structure

All packets — request and response — share the same header:

```
   offset  size  field
   ──────  ────  ──────────────────────────────────────────
    0      1     magic       = 0xA5
    1      1     version     = 0x01
    2      2     seq         (little-endian, u16)
    4      1     opcode
    5      1     flags
    6      2     payload_len (little-endian, u16)
    8      ...   payload (opcode-specific; see §5)
    N      4     crc32       (little-endian, IEEE 802.3 polynomial,
                              computed over offset 0 .. N-1)
```

- `magic` + `version` let a reader resynchronise after a malformed frame and
  reject incompatible protocol versions.
- `seq` is echoed back in every ACK. Host must not reuse a `seq` until the
  previous one has ACKed (strict-serial contract, see §6).
- `flags`:
  - bit 0 — response (0 = request, 1 = response/ACK)
  - bit 1 — last-fragment (set on all single-frame responses and the final
    frame of a multi-frame response; used by `SCREEN`)
  - bit 2 — authenticated (set if the session passed §7 auth)
  - bits 3–7 reserved, must be 0
- `crc32` covers the header and payload, not the COBS-encoded form. Verifies
  end-to-end integrity independently of the USB CRC.

---

## 5. Commands

All opcodes defined for v0.1. Opcode values are stable across versions;
new opcodes append. Unused opcodes MUST be rejected with `ERR_UNKNOWN_OP`.

| Opcode | Name         | Request payload         | Response payload              |
|--------|--------------|-------------------------|-------------------------------|
| 0x01   | `HELLO`      | host version + nonce    | device version + challenge    |
| 0x02   | `AUTH`       | HMAC(challenge, key)    | status                        |
| 0x10   | `KEY`        | keycode, pressed, flags | accepted UI delta hash        |
| 0x11   | `TYPE`       | UTF-8 string            | commit summary                |
| 0x12   | `UI_CMD`     | ui_action enum          | UI delta hash                 |
| 0x20   | `UI_STATE`   | —                       | screen id + focus path        |
| 0x21   | `SCREEN`     | region / crc-only flag  | RGB565 fragments or CRC only  |
| 0x30   | `EVENT_SUB`  | event mask              | previous mask (unsolicited events follow, same seq=0) |
| 0x40   | `LOG_TAIL`   | tail byte offset        | breadcrumb bytes              |
| 0x50   | `MODE_GET`   | —                       | current USB mode              |
| 0xFF   | `ERR`        | (not sent as request)   | error code + reason string    |

> **`MODE_SET` is deliberately omitted.** A Control-driven transition to Mode
> OFF would disconnect CDC#1 mid-session and leave the host with no channel to
> recover — the user would then need to physically interact with the device,
> defeating the remote-debug use case. Mode switching remains a UI-only
> operation.

### 5.1 `KEY` (0x10)

Injects a single key event into the Core 1 `KeyEvent` queue with
`source = INJECT`. The UI layer receives it identically to a hardware key,
with the following differences:

- HW-origin events have priority if they arrive in the same 8 ms debounce
  window (see §4.4 HW-vs-INJECT arbitration).
- Safe mode (§10) rejects all `INJECT` events with `ERR_SAFE_MODE`.

| Field    | Size  | Notes                                                 |
|----------|-------|-------------------------------------------------------|
| keycode  | 1     | Value from `firmware/mie/include/mie/keycode.h`      |
| pressed  | 1     | 0 = release, 1 = press                                |
| flags    | 1     | bit 0 = long-press hint; bits 1–7 reserved           |

Response payload — 4 bytes: UI delta hash (FNV-1a over the view tree post-dispatch).
Host compares before/after hashes to detect whether the keypress was consumed.

ACK timing: sent after the KeyEvent has been drained by `UITask` / `IMETask`
and the next LVGL tick completes. Typical latency: 5–20 ms.

### 5.2 `TYPE` (0x11)

Injects a UTF-8 string bypassing the IME. Each codepoint is delivered as a
synthesised commit event to the currently-focused text widget. Typical use:
testing maximum-length inputs, testing non-ASCII without the IME dance.

| Field    | Size     | Notes                                     |
|----------|----------|-------------------------------------------|
| text_len | 2        | little-endian, u16, bytes (not codepoints)|
| text     | text_len | UTF-8, not null-terminated                |

Response payload:

| Field        | Size  | Notes                                   |
|--------------|-------|-----------------------------------------|
| accepted     | 2     | count of codepoints accepted            |
| rejected     | 2     | count rejected (not focused, full, ...) |
| reason_code  | 1     | 0 = OK; see §6.3 reject codes           |

ACK timing: sent after all codepoints have been dispatched and the UI
has redrawn. May take 50–200 ms for long strings.

### 5.3 `UI_CMD` (0x12)

Semantic navigation, decoupled from physical keys. Intended for tests that
don't care *how* the UI was reached.

| ui_action | u8  | Action                            |
|-----------|-----|-----------------------------------|
| 0x01      | UP                                      |
| 0x02      | DOWN                                    |
| 0x03      | LEFT                                    |
| 0x04      | RIGHT                                   |
| 0x05      | SELECT (same as OK)                     |
| 0x06      | BACK                                    |
| 0x07      | HOME (same as long-BACK)                |
| 0x08      | MENU (same as FUNC)                     |

Response payload — 4 bytes UI delta hash (same as `KEY`).

### 5.4 `UI_STATE` (0x20)

Returns the current UI state in a structured form.

Response payload:

| Field         | Size  | Notes                                    |
|---------------|-------|------------------------------------------|
| screen_id     | 2     | enum from `ui_screens.h`                 |
| focus_depth   | 1     | 0 = root, 1–3 = nested                   |
| focus_path[4] | 4     | u8 index at each depth                   |
| state_hash    | 4     | FNV-1a over full view tree + model state |
| tag_len       | 1     | length of human-readable tag             |
| tag           | tag_len| e.g. "messages.list.row[3]"             |

A test that needs "are we still on the same screen after X" compares
`state_hash`; a test that needs semantic assertions compares `tag`.

### 5.5 `SCREEN` (0x21)

Returns the current framebuffer. Two modes via request flag:

- `crc-only` — returns just the CRC32 of the raw RGB565 buffer (~4 bytes,
  single frame). For visual-regression tests that only need a hash.
- `full` — returns 240×320×RGB565 = 150 KB in up to 10 fragments of 16 KB
  each. Fragment N's header has `flags.bit1 = 0` until the last; all carry
  the same `seq`.

Request payload:

| Field    | Size  | Notes                    |
|----------|-------|--------------------------|
| mode     | 1     | 0 = crc-only, 1 = full   |

Response payload (per fragment, `full` mode):

| Field        | Size     | Notes                                      |
|--------------|----------|--------------------------------------------|
| frag_index   | 1        | 0-based                                    |
| frag_count   | 1        | total fragments for this screen            |
| pixel_offset | 4        | byte offset of this fragment within 150 KB |
| pixels       | up to 16K| raw RGB565                                 |

Hard requirement: `SCREEN` must not block `UITask` beyond one LVGL tick. The
framebuffer is copied under the LVGL mutex into a scratch buffer; the copy
then streams to CDC#1 asynchronously.

### 5.6 `EVENT_SUB` (0x30)

Subscribes the host to unsolicited events. After subscription, the device
sends packets with `seq = 0` and `flags.response = 1`. Host must never send
`seq = 0` in a request.

Event mask bits:

| Bit | Event             | Payload                                  |
|-----|-------------------|------------------------------------------|
| 0   | UI_CHANGE         | screen_id + state_hash                   |
| 1   | IME_COMMIT        | UTF-8 committed string                   |
| 2   | IME_MODE_CHANGE   | new `InputMode` enum                     |
| 3   | POWER_STATE       | new power FSM state                      |
| 4   | WATCHDOG_NEAR     | ms until reset                           |
| 5   | LOG_WRITE         | new log lines since last LOG_TAIL        |

Subscribing with mask `0x00` unsubscribes. The device maintains only one
subscription mask per CDC#1 session; disconnect clears it.

### 5.7 `LOG_TAIL` (0x40)

Reads the tail of the Core 1 breadcrumb ring (FA §9.3).

| Field    | Size | Notes                                                 |
|----------|------|-------------------------------------------------------|
| offset   | 4    | absolute byte offset within ring (0 = read from current tail)|
| max_len  | 2    | cap on returned bytes                                 |

Response: raw UTF-8 lines up to `max_len`, plus next offset for the caller to
resume from. Serves as a CDC#1 alternative to dumping the breadcrumb ring over
CDC#0 during CLI use.

### 5.8 `MODE_GET` (0x50)

Returns current USB mode (`OFF` would not be reachable here by definition —
CDC#1 is dormant in Mode OFF — so the response is always `COMM`; this command
exists for symmetry and version-probing).

---

## 6. ACK Semantics

### 6.1 Strict serial

Host sends one request, waits for one response, then sends the next. The
device processes requests in receive order; there is no pipelining, no
reordering. `seq` monotonically increases (wrap at 2^16 is allowed but
unusual in a single session).

Rationale: the tests we care about are deterministic scripts, not throughput
benchmarks. Pipelining would require per-opcode reentrancy audits and offer
no practical benefit.

### 6.2 "Completion" defined

An ACK is sent only after the requested operation has observable effect:

| Opcode      | ACK sent after                                       |
|-------------|------------------------------------------------------|
| `KEY`       | next LVGL tick after KeyEvent drained                |
| `TYPE`      | all codepoints dispatched and UI redrawn             |
| `UI_CMD`    | next LVGL tick after action processed                |
| `UI_STATE`  | immediate — snapshot under LVGL mutex                |
| `SCREEN`    | all fragments transmitted                            |
| `EVENT_SUB` | mask installed (immediate)                           |
| `LOG_TAIL`  | bytes read from ring (immediate)                     |
| `MODE_GET`  | immediate                                            |

Worst-case `TYPE` with 200 codepoints: ~200 ms. Hosts must use a ≥ 1 s
receive timeout.

### 6.3 Error responses

On failure the device responds with opcode `0xFF` (`ERR`), same `seq`, and
payload:

| Field         | Size  | Notes                                 |
|---------------|-------|---------------------------------------|
| reason_code   | 1     | see table below                       |
| detail_len    | 1     | bytes of human-readable detail        |
| detail        | detail_len| UTF-8, not null-terminated        |

| Code | Symbol              | Meaning                                           |
|------|---------------------|---------------------------------------------------|
| 0x01 | `ERR_UNKNOWN_OP`    | opcode not implemented                            |
| 0x02 | `ERR_BAD_ARG`       | payload failed validation                         |
| 0x03 | `ERR_BAD_CRC`       | crc32 mismatch (framing layer)                    |
| 0x04 | `ERR_BUSY`          | another operation in flight (shouldn't happen under §6.1 but returned defensively) |
| 0x05 | `ERR_UNAUTH`        | request needs prior `AUTH` (see §7)               |
| 0x06 | `ERR_DISABLED`      | runtime gate closed (see §8)                      |
| 0x07 | `ERR_SAFE_MODE`     | device in safe mode (FA §9.4); only `LOG_TAIL`,
|      |                     | `UI_STATE`, `MODE_GET`, `HELLO` allowed           |
| 0x08 | `ERR_TIMEOUT`       | internal deadline exceeded (e.g. UI mutex wait)   |
| 0x09 | `ERR_OOM`           | host asked for a fragment larger than available scratch |
| 0xFE | `ERR_INTERNAL`      | unexpected; `detail` carries a triage tag         |

---

## 7. Authentication

Rationale: the runtime gate (§8) is controlled by the user. Once open, any
host with physical USB access could drive the Control Protocol. For remote
debug (user away from the device, e.g. running a Python harness over a
forwarded serial tunnel), we add a challenge-response layer so a compromised
host on the same LAN cannot silently inject input.

### 7.1 Shared-secret HMAC-SHA256 (v0.1)

- Device stores a 32-byte **control key** in LittleFS. Generated once by the
  user from the Settings UI ("Export pairing QR") or a one-shot CLI. Lost key
  requires physical UI access to regenerate.
- On CDC#1 connect, every command except `HELLO` / `AUTH` / `MODE_GET` returns
  `ERR_UNAUTH` until the session completes a challenge-response.
- `HELLO` request payload: host version (4 bytes).
  `HELLO` response payload: device version (4 bytes) + challenge (16 random
  bytes from `rosc`).
- `AUTH` request payload: 32-byte HMAC-SHA256(challenge, control_key).
  On match, the session is marked authenticated (`flags.bit2` set in all
  subsequent responses). On mismatch, device increments a failure counter —
  3 failures within 60 s close CDC#1 for 5 minutes.

### 7.2 Commands permitted without auth

`HELLO`, `AUTH`, `MODE_GET`, and `LOG_TAIL`. The latter is included so a
host that *forgot the key* can still retrieve the breadcrumb ring for a
remote crash triage — we consider log data non-sensitive relative to the
cost of losing diagnostic access.

All state-mutating commands (`KEY`, `TYPE`, `UI_CMD`, `EVENT_SUB`) require
auth.

---

## 8. Build Flag and Runtime Gate

Two independent layers, both required for Control to function.

### 8.1 Build flag — `MOKYA_ENABLE_USB_CONTROL`

- **ON** (default for all builds currently — no CE/FCC submission planned):
  TinyUSB descriptor declares CDC#1; `UsbCtrlTask` linked in; command handlers
  present.
- **OFF**: descriptor drops CDC#1 (device enumerates as single-CDC COMM);
  `UsbCtrlTask` not built; handler functions not linked. Zero attack surface.

If a future certified shipment requires it, flipping the flag produces a
compliant image with no code-path changes elsewhere.

### 8.2 Runtime gate — `settings.usb_control_enabled`

Default **false**. Even with the build flag ON, the gate starts closed at
every boot.

Opening the gate:

1. User navigates Settings → Developer → USB Control → toggle ON. (Normal
   path for local development.)
2. **Remote-unlock for remote debug**: the user can persist a "auto-enable
   after N minutes uptime" flag from the UI, or pre-authorise a host key
   fingerprint. A remote host presenting a valid HMAC over a device-emitted
   challenge (§7) can then use Control even if the physical UI is inaccessible.
   Rationale: the MokyaLora designer will ship units to field-test locations
   and may need to debug without being physically present.

While closed, any Control command except `HELLO` / `MODE_GET` / `LOG_TAIL`
returns `ERR_DISABLED`.

### 8.3 Safe-mode interaction

Per FA §9.4, safe mode keeps CDC#0 running for Meshtastic admin recovery.
CDC#1 enumerates (build flag is independent of safe mode) but all
state-mutating Control commands return `ERR_SAFE_MODE`. `LOG_TAIL`,
`UI_STATE`, and `MODE_GET` remain available to aid remote diagnosis.

---

## 9. KeyEvent Source Flag

Core 1's `KeyEvent` queue (FA §4.4) is upgraded to multi-producer. Every
event carries a `source` field:

```c
typedef enum {
    KEY_SOURCE_HW     = 0,   // PIO1 scan → KeypadScan task
    KEY_SOURCE_INJECT = 1,   // CDC#1 Control → UsbCtrlTask
} key_source_t;

typedef struct {
    uint8_t       keycode;     // from mie/keycode.h, 0x01..0x3F
    uint8_t       pressed : 1; // 1 = press, 0 = release
    key_source_t  source  : 1;
    uint8_t       flags   : 6; // long-press hint, future use
} key_event_t;
```

### 9.1 HW-vs-INJECT arbitration

Rule: **HW wins within the 20 ms debounce window.**

If a physical key is currently pressed (tracked by `KeypadScan`'s debounce
state) and an `INJECT` press for the same keycode arrives, the inject is
dropped and returns `ERR_BUSY`. Conversely, an inject press that completes
before HW activity proceeds normally; any subsequent HW event for the same
keycode goes through.

Rationale: if the user is physically touching the device, we do not let
a remote host fight for UI control. Also prevents test harnesses from
corrupting results when the test technician brushes the keyboard.

### 9.2 Observability

- `LOG_TAIL` breadcrumbs tag inject events with `INJ:` prefix so a post-mortem
  can distinguish human from automated activity.
- `UI_STATE` responses include a `last_key_source` field (bit flag) updated
  on every consumed key event.

---

## 10. Keycode Table

The canonical keycode definitions live in
[`firmware/mie/include/mie/keycode.h`](../../firmware/mie/include/mie/keycode.h).

Design rules (also recorded in `mie-architecture.md` §7):

- `0x00` = `MOKYA_KEY_NONE` (sentinel for errors and "no key").
- `0x01..0x3F` = compact semantic enumeration; matrix-vs-non-matrix
  distinction does NOT exist at this layer. The power key, future side
  buttons, and matrix keys all share the same namespace.
- Matrix-to-keycode translation is confined to
  `firmware/core1/src/keymap_matrix.h`, applied once inside `KeypadScan`
  before the event enters the queue.

Host tools import the same keycode constants via a Python binding generated
from `keycode.h` (see §11).

---

## 11. Host-side Tooling

### 11.1 `tools/mokya-ctl/` — CLI

Python 3.10+ package that ships alongside the firmware repository.

Entry points:

```
mokya-ctl key <name|code> [--hold MS]
mokya-ctl type "任何 UTF-8 字串"
mokya-ctl nav <up|down|left|right|ok|back|home|menu>
mokya-ctl screen [--crc-only] [--out screenshot.png]
mokya-ctl state [--json]
mokya-ctl log --follow
mokya-ctl auth --key-file ~/.mokya/pairing.key
```

Auto-detects the CDC#1 port by matching USB VID `0x2E8A` + the specific
interface descriptor string `"MokyaLora Control"`. `--port` overrides.

### 11.2 Python binding

`mokya-ctl` is built on a reusable `mokya_control` Python module that exposes:

```python
from mokya_control import ControlSession, Key, UiAction

with ControlSession.auto_detect(key_file="~/.mokya/pairing.key") as s:
    s.press(Key.OK)
    assert s.state().screen_id == ScreenId.HOME
    png_bytes = s.screenshot()
```

The module is independently installable (`pip install -e tools/mokya-ctl`)
and is the supported entry point for `pytest`-based hardware-in-the-loop
regression suites.

The `Key` and `UiAction` enums are generated at build time from
`keycode.h` and `ui_actions.h` respectively — a single source of truth,
firmware and host cannot drift.

---

## 12. Open Items

- **Timing jitter**: `KEY`-ACK timing (§6.2) includes "next LVGL tick",
  which with a 5 ms tick could add up to 5 ms of variance. Acceptable for
  CI-scale testing; revisit if sub-millisecond key-to-hash latency is ever
  required.
- **`TYPE` and focus loss**: spec currently rejects codepoints with
  `reason_code = 1 (not_focused)` if focus changes mid-string. Consider
  whether partial acceptance or full rollback is the correct semantic.
- **Fragment reassembly under disconnect**: if CDC#1 drops mid-`SCREEN`,
  the host has partial pixels and no way to resume. Currently the fix is
  "re-issue the command". A `SCREEN_RESUME` opcode is deferred until the
  use case appears.
