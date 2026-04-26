// keycode.h — MokyaLora canonical keycode enumeration
// SPDX-License-Identifier: MIT
//
// This header is the single source of truth for semantic key identifiers
// used across the MokyaLora firmware stack and host tooling:
//
//   - mie::KeyEvent (firmware/mie/include/mie/hal_port.h)
//   - mie_process_key  (firmware/mie/include/mie/mie.h)
//   - Core 1 key_event_t multi-producer queue  (firmware-architecture §4.6)
//   - USB Control Protocol KEY opcode          (usb-control-protocol §5)
//   - Python Key enum (generated at build time from this file)
//
// Design rules (mie-architecture §7):
//
//   - 0x00            = MOKYA_KEY_NONE (sentinel; never consumed by IME).
//   - 0x01..0x3F      = compact semantic enumeration. Matrix-vs-non-matrix
//                       distinction does NOT exist at this layer — the
//                       power key, future side buttons, and matrix keys all
//                       share the same namespace.
//   - 0x40..0xFF      = reserved; do not use.
//
// Matrix → keycode translation is confined to
// `firmware/core1/src/keymap_matrix.h`, applied once inside the KeypadScan
// task before the event enters the KeyEvent queue. Nothing above KeypadScan
// (MIE, UI, USB Control injection) sees matrix coordinates.
//
// Numeric values assigned here follow the physical keypad row-major order
// (row*6 + col + 1 for the 6×6 matrix); this is an internal numbering
// convention and is NOT part of the public contract — always refer to
// constants by name.

#ifndef MIE_KEYCODE_H
#define MIE_KEYCODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t mokya_keycode_t;

/* Sentinel — used for errors, "no key", and queue-empty poll returns. */
#define MOKYA_KEY_NONE          ((mokya_keycode_t)0x00)

/* ── 6×6 physical keypad (36 keys) ─────────────────────────────────── */
/* Row 0: top tone/digit row + FUNC                                     */
#define MOKYA_KEY_1             ((mokya_keycode_t)0x01)  /* 1 2 / ㄅㄉ */
#define MOKYA_KEY_3             ((mokya_keycode_t)0x02)  /* 3 4 / ˇ ˋ  */
#define MOKYA_KEY_5             ((mokya_keycode_t)0x03)  /* 5 6 / ㄓ ˊ */
#define MOKYA_KEY_7             ((mokya_keycode_t)0x04)  /* 7 8 / ˙ ㄚ */
#define MOKYA_KEY_9             ((mokya_keycode_t)0x05)  /* 9 0 / ㄞㄢㄦ */
#define MOKYA_KEY_FUNC          ((mokya_keycode_t)0x06)

/* Row 1: Bopomofo/Latin input row + SET                                */
#define MOKYA_KEY_Q             ((mokya_keycode_t)0x07)  /* Q W / ㄆㄊ */
#define MOKYA_KEY_E             ((mokya_keycode_t)0x08)  /* E R / ㄍㄐ */
#define MOKYA_KEY_T             ((mokya_keycode_t)0x09)  /* T Y / ㄔㄗ */
#define MOKYA_KEY_U             ((mokya_keycode_t)0x0A)  /* U I / ㄧㄛ */
#define MOKYA_KEY_O             ((mokya_keycode_t)0x0B)  /* O P / ㄟㄣ */
#define MOKYA_KEY_SET           ((mokya_keycode_t)0x0C)

/* Row 2: Bopomofo/Latin input row + BACK                               */
#define MOKYA_KEY_A             ((mokya_keycode_t)0x0D)  /* A S / ㄇㄋ */
#define MOKYA_KEY_D             ((mokya_keycode_t)0x0E)  /* D F / ㄎㄑ */
#define MOKYA_KEY_G             ((mokya_keycode_t)0x0F)  /* G H / ㄕㄘ */
#define MOKYA_KEY_J             ((mokya_keycode_t)0x10)  /* J K / ㄨㄜ */
#define MOKYA_KEY_L             ((mokya_keycode_t)0x11)  /* L   / ㄠㄤ */
#define MOKYA_KEY_BACK          ((mokya_keycode_t)0x12)

/* Row 3: Bopomofo/Latin input row + DEL                                */
#define MOKYA_KEY_Z             ((mokya_keycode_t)0x13)  /* Z X / ㄈㄌ */
#define MOKYA_KEY_C             ((mokya_keycode_t)0x14)  /* C V / ㄏㄒ */
#define MOKYA_KEY_B             ((mokya_keycode_t)0x15)  /* B N / ㄖㄙ */
#define MOKYA_KEY_M             ((mokya_keycode_t)0x16)  /* M   / ㄩㄝ */
#define MOKYA_KEY_BACKSLASH     ((mokya_keycode_t)0x17)  /* \   / ㄡㄥ */
#define MOKYA_KEY_DEL           ((mokya_keycode_t)0x18)

/* Row 4: mode / symbol / tab / space / volume up                       */
#define MOKYA_KEY_MODE          ((mokya_keycode_t)0x19)
#define MOKYA_KEY_TAB           ((mokya_keycode_t)0x1A)
#define MOKYA_KEY_SPACE         ((mokya_keycode_t)0x1B)
#define MOKYA_KEY_SYM1          ((mokya_keycode_t)0x1C)  /* ，SYM (row4 col3) */
#define MOKYA_KEY_SYM2          ((mokya_keycode_t)0x1D)  /* 。.？ (row4 col4) */
#define MOKYA_KEY_VOL_UP        ((mokya_keycode_t)0x1E)

/* Row 5: navigation + OK + volume down                                 */
#define MOKYA_KEY_UP            ((mokya_keycode_t)0x1F)
#define MOKYA_KEY_DOWN          ((mokya_keycode_t)0x20)
#define MOKYA_KEY_LEFT          ((mokya_keycode_t)0x21)
#define MOKYA_KEY_RIGHT         ((mokya_keycode_t)0x22)
#define MOKYA_KEY_OK            ((mokya_keycode_t)0x23)
#define MOKYA_KEY_VOL_DOWN      ((mokya_keycode_t)0x24)

/* ── Non-matrix keys (0x25..0x3F reserved) ──────────────────────────── */
/* Power button — side-mounted, independent of the matrix scan.         */
#define MOKYA_KEY_POWER         ((mokya_keycode_t)0x25)

/* Upper bound for valid, defined keycodes. Values ≥ MOKYA_KEY_LIMIT    *
 * are reserved for future extension and must not be emitted.           */
#define MOKYA_KEY_LIMIT         ((mokya_keycode_t)0x40)

/* ── Producer-side event flags (Phase 1.4 long-press disambiguation) ── *
 * Carried in key_event_t.flags (Core 1 queue) and mie::KeyEvent::flags
 * (engine-facing). Only meaningful on SmartZh slot keys; other consumers
 * may set the bits but the engine ignores them where irrelevant.        *
 *                                                                       *
 *   MOKYA_KEY_FLAG_LONG_PRESS — key held past kLongPressMs (500 ms);    *
 *                               consume secondary phoneme (strict).     *
 *                                                                       *
 * Two-state design: a short tap means "fuzzy / any phoneme on this      *
 * slot" (legacy half-keyboard), and a long press means "strict          *
 * secondary phoneme". Slot 4's tertiary (ㄦ) is reachable only via the  *
 * fuzzy short-tap path.                                                  *
 *                                                                       *
 * Bits 1..5 reserved for future hint flags.                              */
#define MOKYA_KEY_FLAG_LONG_PRESS   ((uint8_t)0x01)
/* Test/debug only: force the engine to record hint = ANY (0xFF) for this
 * byte. Equivalent to a short tap under the current default semantics —
 * kept as an explicit signal so test scripts can pin the behaviour even
 * if the engine default ever changes. */
#define MOKYA_KEY_FLAG_HINT_ANY     ((uint8_t)0x04)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MIE_KEYCODE_H */
