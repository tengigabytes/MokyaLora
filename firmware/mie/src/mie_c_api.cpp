// mie_c_api.cpp — MokyaInput Engine C API implementation
// SPDX-License-Identifier: MIT
//
// Thin wrapper: mie_dict_t owns a mie::TrieSearcher;
//               mie_ctx_t  owns a mie::ImeLogic.
// Both are heap-allocated so they can be treated as opaque pointers from C.

#include <mie/mie.h>
#include <mie/trie_searcher.h>
#include <mie/ime_logic.h>
#include <new>

// ── Opaque struct definitions ─────────────────────────────────────────────────

struct mie_dict_s {
    mie::TrieSearcher searcher;
};

struct mie_ctx_s {
    mie_ctx_s(mie_dict_t* zh, mie_dict_t* en)
        : logic(zh->searcher, en ? &en->searcher : nullptr) {}
    mie::ImeLogic logic;
};

// ── Dictionary ────────────────────────────────────────────────────────────────

mie_dict_t* mie_dict_open(const char* dat_path, const char* val_path) {
    mie_dict_t* d = new(std::nothrow) mie_dict_t;
    if (!d) return nullptr;
    if (!d->searcher.load_from_file(dat_path, val_path)) {
        delete d;
        return nullptr;
    }
    return d;
}

mie_dict_t* mie_dict_open_memory(const uint8_t* dat_buf, size_t dat_size,
                                  const uint8_t* val_buf, size_t val_size) {
    mie_dict_t* d = new(std::nothrow) mie_dict_t;
    if (!d) return nullptr;
    if (!d->searcher.load_from_memory(dat_buf, dat_size, val_buf, val_size)) {
        delete d;
        return nullptr;
    }
    return d;
}

void mie_dict_close(mie_dict_t* dict) {
    delete dict;  // delete nullptr is a no-op
}

// ── Context ───────────────────────────────────────────────────────────────────

mie_ctx_t* mie_ctx_create(mie_dict_t* zh, mie_dict_t* en) {
    if (!zh) return nullptr;
    return new(std::nothrow) mie_ctx_s(zh, en);
}

void mie_ctx_destroy(mie_ctx_t* ctx) {
    delete ctx;
}

void mie_set_commit_cb(mie_ctx_t* ctx,
                        void (*cb)(const char* utf8, void* user_data),
                        void* user_data) {
    if (ctx) ctx->logic.set_commit_callback(cb, user_data);
}

int mie_process_key(mie_ctx_t* ctx, uint8_t row, uint8_t col, int pressed) {
    if (!ctx) return 0;
    mie::KeyEvent ev;
    ev.row     = row;
    ev.col     = col;
    ev.pressed = (pressed != 0);
    return ctx->logic.process_key(ev) ? 1 : 0;
}

void mie_clear_input(mie_ctx_t* ctx) {
    if (ctx) ctx->logic.clear_input();
}

// ── Display ───────────────────────────────────────────────────────────────────

const char* mie_input_str(mie_ctx_t* ctx) {
    return ctx ? ctx->logic.input_str() : "";
}

const char* mie_compound_str(mie_ctx_t* ctx) {
    return ctx ? ctx->logic.compound_input_str() : "";
}

const char* mie_mode_indicator(mie_ctx_t* ctx) {
    return ctx ? ctx->logic.mode_indicator() : "";
}

// ── Candidates ────────────────────────────────────────────────────────────────

int mie_candidate_count(mie_ctx_t* ctx) {
    return ctx ? ctx->logic.merged_candidate_count() : 0;
}

const char* mie_candidate_word(mie_ctx_t* ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->logic.merged_candidate_count())
        return nullptr;
    return ctx->logic.merged_candidate(idx).word;
}

int mie_candidate_lang(mie_ctx_t* ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->logic.merged_candidate_count())
        return -1;
    return ctx->logic.merged_candidate_lang(idx);
}

// ── Pagination ────────────────────────────────────────────────────────────────

int mie_page_size(void) {
    return mie::ImeLogic::kCandPageSize;
}

int mie_cand_page(mie_ctx_t* ctx) {
    return ctx ? ctx->logic.cand_page() : 0;
}

int mie_cand_page_count(mie_ctx_t* ctx) {
    return ctx ? ctx->logic.cand_page_count() : 0;
}

int mie_page_cand_count(mie_ctx_t* ctx) {
    return ctx ? ctx->logic.page_cand_count() : 0;
}

const char* mie_page_cand_word(mie_ctx_t* ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->logic.page_cand_count())
        return nullptr;
    return ctx->logic.page_cand(idx).word;
}

int mie_page_cand_lang(mie_ctx_t* ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->logic.page_cand_count())
        return -1;
    return ctx->logic.page_cand_lang(idx);
}

int mie_page_sel(mie_ctx_t* ctx) {
    return ctx ? ctx->logic.page_sel() : 0;
}
