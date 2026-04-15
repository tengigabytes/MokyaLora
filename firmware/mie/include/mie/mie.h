// mie.h — MokyaInput Engine C API
// SPDX-License-Identifier: MIT
//
// Stable C interface (opaque handles, C linkage) for use from:
//   Android JNI  — mie_process_key / mie_candidate_word / commit callback
//   Windows TSF  — COM ITextInputProcessor thin wrapper
//   MokyaLora    — RP2350 firmware (may use C++ API directly instead)
//
// Lifecycle:
//   1. mie_dict_open / mie_dict_open_memory  → mie_dict_t*
//   2. mie_ctx_create(zh, en)                → mie_ctx_t*
//   3. mie_set_commit_cb(ctx, cb, userdata)
//   4. mie_process_key(...)  — feed key events; redraw UI when return != 0
//   5. mie_ctx_destroy  →  mie_dict_close
//
//   The dictionary handle(s) must outlive any context created from them.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include <mie/keycode.h>

// ── Opaque handles ────────────────────────────────────────────────────────────

/** Opaque dictionary handle (wraps mie::TrieSearcher). */
typedef struct mie_dict_s mie_dict_t;

/** Opaque IME context handle (wraps mie::ImeLogic). */
typedef struct mie_ctx_s mie_ctx_t;

// ── Dictionary ────────────────────────────────────────────────────────────────

/** Open a dictionary from files on disk (PC / host).
 *  Returns NULL if the files cannot be read or are malformed. */
mie_dict_t* mie_dict_open(const char* dat_path, const char* val_path);

/** Open a dictionary from in-memory buffers (embedded / PSRAM).
 *  The buffers must remain valid and unmodified for the lifetime of the handle.
 *  Returns NULL if the header is malformed. */
mie_dict_t* mie_dict_open_memory(const uint8_t* dat_buf, size_t dat_size,
                                  const uint8_t* val_buf, size_t val_size);

/** Close the dictionary and free all resources.  NULL is a no-op. */
void mie_dict_close(mie_dict_t* dict);

// ── Context ───────────────────────────────────────────────────────────────────

/** Create an IME context.
 *  zh — required Chinese dictionary; must not be NULL.
 *  en — optional English dictionary; NULL disables English candidates.
 *  Returns NULL on allocation failure or if zh is NULL. */
mie_ctx_t* mie_ctx_create(mie_dict_t* zh, mie_dict_t* en);

/** Destroy an IME context and free all resources.  NULL is a no-op. */
void mie_ctx_destroy(mie_ctx_t* ctx);

/** Register a commit callback.
 *  cb        — called whenever text is confirmed (UTF-8, null-terminated).
 *  user_data — forwarded as the second argument to cb.
 *  Passing cb=NULL disables the callback. */
void mie_set_commit_cb(mie_ctx_t* ctx,
                        void (*cb)(const char* utf8, void* user_data),
                        void* user_data);

/** Feed one key event.
 *  keycode — semantic keycode from <mie/keycode.h> (MOKYA_KEY_*, 0x01..0x3F).
 *            MOKYA_KEY_NONE (0x00) and codes ≥ MOKYA_KEY_LIMIT are ignored.
 *  pressed — non-zero = key-down; zero = key-up.
 *  Returns non-zero if the UI should be refreshed (display or candidates changed). */
int mie_process_key(mie_ctx_t* ctx, uint8_t keycode, int pressed);

/** Clear all input state (resets buffer, candidates, mode stays unchanged). */
void mie_clear_input(mie_ctx_t* ctx);

// ── Display ───────────────────────────────────────────────────────────────────

/** Current input display string (UTF-8, null-terminated).
 *  SmartZh — accumulated Bopomofo phonemes.
 *  SmartEn — accumulated letters.
 *  Direct  — current pending label.
 *  Returns "" for a NULL context.
 *  Pointer is valid until the next call to any mie_* function on this context. */
const char* mie_input_str(mie_ctx_t* ctx);

/** Compound display string.
 *  SmartZh — "[ph0ph1]ˉ" notation.  Others — same as mie_input_str().
 *  Returns "" for a NULL context. */
const char* mie_compound_str(mie_ctx_t* ctx);

/** Short mode label: "中" / "EN" / "ABC" / "abc" / "ㄅ".
 *  Returns "" for a NULL context. */
const char* mie_mode_indicator(mie_ctx_t* ctx);

// ── Candidates (merged ZH+EN interleaved list) ────────────────────────────────

/** Total number of candidates across all pages. */
int mie_candidate_count(mie_ctx_t* ctx);

/** UTF-8 word for the idx-th candidate (0-based, over the full merged list).
 *  Returns NULL if idx is out of range or ctx is NULL. */
const char* mie_candidate_word(mie_ctx_t* ctx, int idx);

/** Language of the idx-th candidate: 0 = Chinese, 1 = English.
 *  Returns -1 if idx is out of range or ctx is NULL. */
int mie_candidate_lang(mie_ctx_t* ctx, int idx);

// ── Pagination ────────────────────────────────────────────────────────────────

/** Number of candidates shown per page (fixed at 5). */
int mie_page_size(void);

/** Current page number (0-indexed). */
int mie_cand_page(mie_ctx_t* ctx);

/** Total number of candidate pages (0 when no candidates). */
int mie_cand_page_count(mie_ctx_t* ctx);

/** Number of candidates on the current page (≤ page_size; may be less on last page). */
int mie_page_cand_count(mie_ctx_t* ctx);

/** UTF-8 word for the idx-th candidate on the current page.
 *  Returns NULL if idx is out of range or ctx is NULL. */
const char* mie_page_cand_word(mie_ctx_t* ctx, int idx);

/** Language of the idx-th candidate on the current page (0=ZH, 1=EN, -1=OOB). */
int mie_page_cand_lang(mie_ctx_t* ctx, int idx);

/** Currently selected index within the current page (0..page_size-1). */
int mie_page_sel(mie_ctx_t* ctx);

#ifdef __cplusplus
}
#endif
