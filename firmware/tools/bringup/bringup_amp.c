#include "bringup.h"
#include "i2s_out.pio.h"
#include "pdm_mic.pio.h"
#if BRINGUP_WAV
#include "voice_pcm.h"
#include "gc_pcm.h"
#include "test01_8k_pcm.h"
#include "test01_16k_pcm.h"
#include "test01_44k_pcm.h"
#include "test01_48k_pcm.h"
#endif

// ---------------------------------------------------------------------------
// NAU8315 I2S audio via PIO
// ---------------------------------------------------------------------------

// Unit sine table (full scale) — computed once at startup using hardware FPU
static int16_t sine_unit[TONE_TABLE_LEN];

void precompute_sine(void) {
    for (int i = 0; i < TONE_TABLE_LEN; i++)
        sine_unit[i] = (int16_t)(32767 * sinf(2.0f * (float)M_PI * i / TONE_TABLE_LEN));
}

// Fast fill: integer scale only — no trig in playback path
static void amp_fill_sine(uint32_t *buf, int amp) {
    for (int i = 0; i < TONE_TABLE_LEN; i++) {
        int16_t s = (int16_t)((int32_t)sine_unit[i] * amp / 32767);
        buf[i] = ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;
    }
}

// Start PIO I2S state machine. Returns SM number, or -1 on failure.
// Program uses .origin 0 — must load at offset 0 (absolute JMP targets).
int amp_pio_start(void) {
    int sm_num = pio_claim_unused_sm(pio0, false);
    if (sm_num < 0) { printf("ERROR: no free PIO SM\n"); return -1; }
    uint sm = (uint)sm_num;

    // RP2350B has 48 GPIOs; PIO addresses 32 at a time.
    // GPIO 32 (DAC) needs gpio_base = 16 → must be set BEFORE add_program.
    pio_set_gpio_base(pio0, 16);

    if (!pio_can_add_program_at_offset(pio0, &i2s_out_program, 0)) {
        printf("ERROR: PIO offset 0 not available\n");
        pio_sm_unclaim(pio0, sm);
        return -1;
    }
    pio_add_program_at_offset(pio0, &i2s_out_program, 0);

    printf("  PIO0 SM%d loaded at offset=0\n", sm);

    i2s_out_program_init(pio0, sm, 0, AMP_DAC_PIN, AMP_BCLK_PIN, 40.0f);
    pio_sm_set_enabled(pio0, sm, true);
    return (int)sm;
}

void amp_pio_stop(int sm) {
    pio_sm_set_enabled(pio0, (uint)sm, false);
    pio_remove_program(pio0, &i2s_out_program, 0);
    pio_sm_unclaim(pio0, (uint)sm);
    gpio_set_function(AMP_BCLK_PIN, GPIO_FUNC_NULL);
    gpio_set_function(AMP_LRCK_PIN, GPIO_FUNC_NULL);
    gpio_set_function(AMP_DAC_PIN,  GPIO_FUNC_NULL);
}

// Constant-amplitude tone for 5 s — use this first to verify hardware
void amp_test(void) {
    // 80 % amplitude: P = 0.68 × 0.64 ≈ 0.44 W — under 0.7 W speaker limit
    const int amp = (int)(MAX_AMPLITUDE * 1.6f);
    printf("\n--- NAU8315 Tone Test (%d Hz, amp 80%%, ~0.44 W) ---\n", TONE_FREQ_HZ);
    printf("    PIO uses standard I2S -- FSL floating is correct, no hardware mod needed.\n");

    int sm = amp_pio_start();
    if (sm < 0) return;

    uint32_t buf[TONE_TABLE_LEN];
    amp_fill_sine(buf, amp);

    uint32_t end_ms = to_ms_since_boot(get_absolute_time()) + 5000;
    int count = 0;
    while (to_ms_since_boot(get_absolute_time()) < end_ms) {
        if (back_key_pressed()) break;
        for (int i = 0; i < TONE_TABLE_LEN; i++)
            pio_sm_put_blocking(pio0, (uint)sm, buf[i]);
        count++;
    }
    printf("  played %d periods\n", count);
    sleep_ms(10);
    amp_pio_stop(sm);
    printf("Amp off\n");
}

// Amplitude-modulated tone — breathing effect × 5
void amp_breathe(void) {
    printf("\n--- NAU8315 Amp Breathe (~%d Hz) ---\n", TONE_FREQ_HZ);

    int sm = amp_pio_start();
    if (sm < 0) return;

    uint32_t buf[TONE_TABLE_LEN];
    for (int b = 0; b < 5; b++) {
        if (back_key_pressed()) break;
        for (int step = 0; step <= 20; step++) {
            amp_fill_sine(buf, MAX_AMPLITUDE * step / 20);
            for (int rep = 0; rep < 10; rep++)
                for (int i = 0; i < TONE_TABLE_LEN; i++)
                    pio_sm_put_blocking(pio0, (uint)sm, buf[i]);
        }
        for (int step = 20; step >= 0; step--) {
            amp_fill_sine(buf, MAX_AMPLITUDE * step / 20);
            for (int rep = 0; rep < 10; rep++)
                for (int i = 0; i < TONE_TABLE_LEN; i++)
                    pio_sm_put_blocking(pio0, (uint)sm, buf[i]);
        }
        printf("  breath %d done\n", b + 1);
    }

    sleep_ms(10);
    amp_pio_stop(sm);
    printf("Amp off\n");
}

// Fill FIFO with zeros — prevents click when transitioning to/from silence.
static void bee_silence(int sm, int dur_ms) {
    uint32_t end_ms = to_ms_since_boot(get_absolute_time()) + (uint32_t)dur_ms;
    while (to_ms_since_boot(get_absolute_time()) < end_ms)
        pio_sm_put_blocking(pio0, (uint)sm, 0);
}

// Play one note: sound for (dur_ms - GAP) ms, then GAP ms of silence.
// freq = 0 → pure silence for dur_ms.
void bee_note(int sm, int freq, int dur_ms, int amp) {
    const int GAP = 45;
    if (freq == 0) { bee_silence(sm, dur_ms); return; }
    int tlen = I2S_SAMPLE_RATE / freq;
    if (tlen > 200) tlen = 200;
    uint32_t buf[200];
    for (int i = 0; i < tlen; i++) {
        int16_t s = (int16_t)(amp * sinf(2.0f * (float)M_PI * i / tlen));
        buf[i] = ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;
    }
    int play_ms = dur_ms > GAP + 20 ? dur_ms - GAP : dur_ms;
    uint32_t end_ms = to_ms_since_boot(get_absolute_time()) + (uint32_t)play_ms;
    while (to_ms_since_boot(get_absolute_time()) < end_ms)
        for (int i = 0; i < tlen; i++)
            pio_sm_put_blocking(pio0, (uint)sm, buf[i]);
    bee_silence(sm, GAP);
}

// ---------------------------------------------------------------------------
// IM69D130 PDM microphone test
//
// Uses pio1 (not pio0) to avoid gpio_base conflict with the amp (pio0, base=16).
// GPIO 4/5 are within pio1's default range (0–31); no base change needed.
//
// Procedure:
//   1. Start PDM CLK at 3.125 MHz (clkdiv=20), collect bits into RX FIFO.
//   2. Discard 100 ms warm-up (mic datasheet: power-up settling).
//   3. Capture 1024 words (32 768 PDM bits ≈ 10.5 ms of audio data).
//   4. Count 1-density and check for stuck-high / stuck-low faults.
//   Silent room: density ≈ 50 %. Loud sound shifts density up or down.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// WAV playback — voice_pcm[] embedded in flash (generated by gen_voice_pcm.py)
// Set BRINGUP_WAV=1 in bringup.h and uncomment WAV sources in CMakeLists.txt to enable.
// ---------------------------------------------------------------------------

#if BRINGUP_WAV
void amp_voice(void) {
    printf("\n--- Voice WAV playback (%u samples @ %u Hz, %.2f s) ---\n",
           (unsigned)voice_pcm_len, (unsigned)VOICE_PCM_RATE,
           (float)voice_pcm_len / (float)VOICE_PCM_RATE);

    int sm = amp_pio_start();
    if (sm < 0) return;

    for (uint32_t i = 0; i < voice_pcm_len; i++) {
        int16_t s = voice_pcm[i];
        pio_sm_put_blocking(pio0, (uint)sm,
                            ((uint32_t)(uint16_t)s << 16) | (uint16_t)s);
    }

    // Drain: a few zero samples to suppress click on stop
    for (int i = 0; i < 256; i++)
        pio_sm_put_blocking(pio0, (uint)sm, 0);

    amp_pio_stop(sm);
    printf("Done\n");
}

// Generic WAV playback helper — applies >>1 gain for speaker safety
static void amp_play(int sm, const int16_t *pcm, uint32_t len,
                     const char *label, uint32_t src_rate) {
    printf("  Playing: %-14s  src=%5u Hz  %u samples  %.1f s\n",
           label, (unsigned)src_rate, (unsigned)len,
           (float)len / (float)VOICE_PCM_RATE);
    for (uint32_t i = 0; i < len; i++) {
        int16_t s = (int16_t)(pcm[i] >> 1);
        pio_sm_put_blocking(pio0, (uint)sm,
                            ((uint32_t)(uint16_t)s << 16) | (uint16_t)s);
    }
    for (int i = 0; i < 256; i++)
        pio_sm_put_blocking(pio0, (uint)sm, 0);
}

void amp_test01_8k(void) {
    printf("\n--- test01 @ 8 kHz src ---\n");
    int sm = amp_pio_start(); if (sm < 0) return;
    amp_play(sm, test01_8k_pcm, test01_8k_pcm_len, "8k->48828", TEST01_8K_PCM_SRC_RATE);
    amp_pio_stop(sm); printf("Done\n");
}
void amp_test01_16k(void) {
    printf("\n--- test01 @ 16 kHz src ---\n");
    int sm = amp_pio_start(); if (sm < 0) return;
    amp_play(sm, test01_16k_pcm, test01_16k_pcm_len, "16k->48828", TEST01_16K_PCM_SRC_RATE);
    amp_pio_stop(sm); printf("Done\n");
}
void amp_test01_44k(void) {
    printf("\n--- test01 @ 44.1 kHz src ---\n");
    int sm = amp_pio_start(); if (sm < 0) return;
    amp_play(sm, test01_44k_pcm, test01_44k_pcm_len, "44k->48828", TEST01_44K_PCM_SRC_RATE);
    amp_pio_stop(sm); printf("Done\n");
}
void amp_test01_48k(void) {
    printf("\n--- test01 @ 48 kHz src ---\n");
    int sm = amp_pio_start(); if (sm < 0) return;
    amp_play(sm, test01_48k_pcm, test01_48k_pcm_len, "48k->48828", TEST01_48K_PCM_SRC_RATE);
    amp_pio_stop(sm); printf("Done\n");
}

// Play all four in sequence for direct A/B/C/D comparison
void amp_test01_all(void) {
    printf("\n--- test01 comparison: 8k / 16k / 44k / 48k (all resampled to 48828 Hz) ---\n");
    int sm = amp_pio_start(); if (sm < 0) return;
    amp_play(sm, test01_8k_pcm,  test01_8k_pcm_len,  "8k->48828",  TEST01_8K_PCM_SRC_RATE);
    amp_play(sm, test01_16k_pcm, test01_16k_pcm_len, "16k->48828", TEST01_16K_PCM_SRC_RATE);
    amp_play(sm, test01_44k_pcm, test01_44k_pcm_len, "44k->48828", TEST01_44K_PCM_SRC_RATE);
    amp_play(sm, test01_48k_pcm, test01_48k_pcm_len, "48k->48828", TEST01_48K_PCM_SRC_RATE);
    amp_pio_stop(sm); printf("Done\n");
}

void amp_gc(void) {
    printf("\n--- GC WAV playback (%u samples @ %u Hz, %.2f s) ---\n",
           (unsigned)gc_pcm_len, (unsigned)GC_PCM_RATE,
           (float)gc_pcm_len / (float)GC_PCM_RATE);
    printf("  Gain: 50%% (>>1) — peak clamped to MAX_AMPLITUDE for speaker safety\n");

    int sm = amp_pio_start();
    if (sm < 0) return;

    for (uint32_t i = 0; i < gc_pcm_len; i++) {
        int16_t s = (int16_t)(gc_pcm[i] >> 1);   // -6 dB: keep within MAX_AMPLITUDE
        pio_sm_put_blocking(pio0, (uint)sm,
                            ((uint32_t)(uint16_t)s << 16) | (uint16_t)s);
    }
    for (int i = 0; i < 256; i++)
        pio_sm_put_blocking(pio0, (uint)sm, 0);

    amp_pio_stop(sm);
    printf("Done\n");
}
#endif  // BRINGUP_WAV

// Little Bee (小蜜蜂) melody — 40% amplitude
//
// C major, 4/4, quarter = 460 ms (~130 BPM).
// Numbered notation (jianpu):
//   5 3 3- | 4 2 2- | 5 3 3- | 4 2 2- | 1 3 5 5 | 3---  |
//   2 2 2 2 | 2 3 4- | 3 3 3 3 | 3 4 5- |
//   5 3 3- | 4 2 2- | 1 3 5 5 | 1---
void amp_bee(void) {
    printf("\n--- Little Bee at 40%% amp ---\n");
    int sm = amp_pio_start();
    if (sm < 0) return;

    const int amp = (int)(MAX_AMPLITUDE * 0.8f);
    const int q = 460, h = 920, w = 1840;   // quarter, half, whole (ms)
    const int C = NOTE_C4, D = NOTE_D4, E = NOTE_E4,
              F = NOTE_F4, G = NOTE_G4;

    // 5 3 3- | 4 2 2- | 5 3 3- | 4 2 2-
    bee_note(sm,G,q,amp); bee_note(sm,E,q,amp); bee_note(sm,E,h,amp);
    bee_note(sm,F,q,amp); bee_note(sm,D,q,amp); bee_note(sm,D,h,amp);
    if (back_key_pressed()) goto bee_done;
    bee_note(sm,G,q,amp); bee_note(sm,E,q,amp); bee_note(sm,E,h,amp);
    bee_note(sm,F,q,amp); bee_note(sm,D,q,amp); bee_note(sm,D,h,amp);

    // 1 3 5 5 | 3---
    if (back_key_pressed()) goto bee_done;
    bee_note(sm,C,q,amp); bee_note(sm,E,q,amp); bee_note(sm,G,q,amp); bee_note(sm,G,q,amp);
    bee_note(sm,E,w,amp);

    // 2 2 2 2 | 2 3 4-
    if (back_key_pressed()) goto bee_done;
    bee_note(sm,D,q,amp); bee_note(sm,D,q,amp); bee_note(sm,D,q,amp); bee_note(sm,D,q,amp);
    bee_note(sm,D,q,amp); bee_note(sm,E,q,amp); bee_note(sm,F,h,amp);

    // 3 3 3 3 | 3 4 5-
    if (back_key_pressed()) goto bee_done;
    bee_note(sm,E,q,amp); bee_note(sm,E,q,amp); bee_note(sm,E,q,amp); bee_note(sm,E,q,amp);
    bee_note(sm,E,q,amp); bee_note(sm,F,q,amp); bee_note(sm,G,h,amp);

    // 5 3 3- | 4 2 2- | 1 3 5 5 | 1---
    if (back_key_pressed()) goto bee_done;
    bee_note(sm,G,q,amp); bee_note(sm,E,q,amp); bee_note(sm,E,h,amp);
    bee_note(sm,F,q,amp); bee_note(sm,D,q,amp); bee_note(sm,D,h,amp);
    bee_note(sm,C,q,amp); bee_note(sm,E,q,amp); bee_note(sm,G,q,amp); bee_note(sm,G,q,amp);
    bee_note(sm,C,w,amp);

bee_done:
    bee_silence(sm, 50);
    amp_pio_stop(sm);
    printf("Done\n");
}
