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
static int amp_pio_start(void) {
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

static void amp_pio_stop(int sm) {
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
static void bee_note(int sm, int freq, int dur_ms, int amp) {
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

void mic_test(void) {
    printf("\n--- IM69D130 PDM Mic Test (GPIO%d=CLK, GPIO%d=DATA) ---\n",
           MIC_CLK_PIN, MIC_DATA_PIN);
    printf("  CLK = %.3f kHz (clkdiv=%.0f)  SELECT=VDD (R-ch, falling edge)\n",
           125000.0f / (2.0f * PDM_CLK_DIV), PDM_CLK_DIV);

    int sm_num = pio_claim_unused_sm(pio1, false);
    if (sm_num < 0) { printf("ERROR: no free PIO1 SM\n"); return; }
    uint sm = (uint)sm_num;

    if (!pio_can_add_program(pio1, &pdm_mic_program)) {
        printf("ERROR: PIO1 program space full\n");
        pio_sm_unclaim(pio1, sm);
        return;
    }
    uint offset = pio_add_program(pio1, &pdm_mic_program);
    pdm_mic_program_init(pio1, sm, offset, MIC_CLK_PIN, MIC_DATA_PIN, PDM_CLK_DIV);
    pio_sm_set_enabled(pio1, sm, true);

    // Warm-up: discard first 100 ms
    uint32_t warmup_end = to_ms_since_boot(get_absolute_time()) + 100;
    while (to_ms_since_boot(get_absolute_time()) < warmup_end)
        (void)pio_sm_get_blocking(pio1, sm);

    // Capture 1024 words = 32 768 PDM bits (autopush at 32 bits per word)
    const int N_WORDS = 1024;
    uint32_t total_ones = 0;
    uint32_t min_word = 0xFFFFFFFFu, max_word = 0;

    for (int i = 0; i < N_WORDS; i++) {
        uint32_t w = pio_sm_get_blocking(pio1, sm);
        uint32_t ones = (uint32_t)__builtin_popcount(w);
        total_ones += ones;
        if (w < min_word) min_word = w;
        if (w > max_word) max_word = w;
    }

    const uint32_t total_bits = (uint32_t)N_WORDS * 32u;
    float density = (float)total_ones / (float)total_bits * 100.0f;

    printf("  Captured %u PDM bits (%d words)\n", total_bits, N_WORDS);
    printf("  1-density : %.1f %%  (silent room ≈ 50%%)\n", density);
    printf("  Min word  : 0x%08X\n", (unsigned)min_word);
    printf("  Max word  : 0x%08X\n", (unsigned)max_word);

    bool stuck_low  = (total_ones == 0);
    bool stuck_high = (total_ones == total_bits);
    bool varying    = (min_word != max_word);

    if (stuck_low)
        printf("  WARN: DATA stuck LOW  — check GPIO%d wiring / VDD\n", MIC_DATA_PIN);
    else if (stuck_high)
        printf("  WARN: DATA stuck HIGH — check GPIO%d pullup or SELECT pin\n", MIC_DATA_PIN);
    else if (!varying)
        printf("  WARN: all words identical (0x%08X) — data may be frozen\n", (unsigned)min_word);

    bool pass = !stuck_low && !stuck_high && varying;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    pio_sm_set_enabled(pio1, sm, false);
    pio_remove_program(pio1, &pdm_mic_program, offset);
    pio_sm_unclaim(pio1, sm);
    gpio_set_function(MIC_CLK_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(MIC_DATA_PIN, GPIO_FUNC_NULL);
}

// ---------------------------------------------------------------------------
// mic_raw — continuous PDM density monitor, no amp, 10 s (Enter to stop)
//
// Every ~0.5 s prints: ones min/max per 64-bit frame, and average density.
// Speak, clap, or whistle to see density shift away from 50%.
// ---------------------------------------------------------------------------

void mic_raw(void) {
    printf("\n--- PDM raw monitor (no amp, 10 s, Enter to stop) ---\n");
    printf("  Silence ≈ 50%%.  Loud sound shifts density up or down.\n");

    int sm_num = pio_claim_unused_sm(pio1, false);
    if (sm_num < 0) { printf("ERROR: no free PIO1 SM\n"); return; }
    uint sm = (uint)sm_num;

    if (!pio_can_add_program(pio1, &pdm_mic_program)) {
        printf("ERROR: PIO1 program space full\n");
        pio_sm_unclaim(pio1, sm);
        return;
    }
    uint offset = pio_add_program(pio1, &pdm_mic_program);
    pdm_mic_program_init(pio1, sm, offset, MIC_CLK_PIN, MIC_DATA_PIN, PDM_CLK_DIV);
    pio_sm_set_enabled(pio1, sm, true);

    // Warm-up: 100 ms
    uint32_t t = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) < t + 100)
        (void)pio_sm_get_blocking(pio1, sm);

    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}

    // Print header
    printf("  %6s  %8s  %4s  %4s  %6s\n", "t(ms)", "samples", "omin", "omax", "density");

    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 10000;
    const int INTERVAL = 24414;  // ~0.5 s at 48828 Hz (2 words = 64 PDM bits per sample)

    while (to_ms_since_boot(get_absolute_time()) < deadline) {
        if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) break;

        int ones_min = 64, ones_max = 0;
        uint32_t total_ones = 0;

        for (int i = 0; i < INTERVAL; i++) {
            uint32_t w0 = pio_sm_get_blocking(pio1, sm);
            uint32_t w1 = pio_sm_get_blocking(pio1, sm);
            int ones = __builtin_popcount(w0) + __builtin_popcount(w1);
            if (ones < ones_min) ones_min = ones;
            if (ones > ones_max) ones_max = ones;
            total_ones += (uint32_t)ones;
        }

        float density = (float)total_ones / (float)(INTERVAL * 64) * 100.0f;
        printf("  %6u  %8d  %4d  %4d  %5.1f%%\n",
               (unsigned)to_ms_since_boot(get_absolute_time()),
               INTERVAL, ones_min, ones_max, density);
    }

    pio_sm_set_enabled(pio1, sm, false);
    pio_remove_program(pio1, &pdm_mic_program, offset);
    pio_sm_unclaim(pio1, sm);
    gpio_set_function(MIC_CLK_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(MIC_DATA_PIN, GPIO_FUNC_NULL);
    printf("Done\n");
}

// ---------------------------------------------------------------------------
// Microphone → Speaker loopback
//
// PDM CLK = 3.125 MHz (clkdiv=20), decimation ratio = 64.
// PIO autopush = 32 bits; software reads 2 words (64 PDM bits) per audio sample.
// 3,125,000 / 32 / 2 = 48,828 Hz — exactly matches the I2S sample rate.
//
// Decimation: simple integrate-and-dump (1-stage CIC).
//   64 PDM bits → ones count [0..64], centre 32.
//   PCM = (ones − 32) × 1024 × GAIN, clamped to MAX_AMPLITUDE for speaker safety.
//
// Audio quality: passable for voice; CIC roll-off above ~10 kHz is expected.
// pio0: I2S amp (gpio_base=16 for GPIO 32). pio1: PDM mic (GPIO 4/5, default base).
// ---------------------------------------------------------------------------

#define MIC_GAIN 4   // 12 dB — speech signal fits within MAX_AMPLITUDE after IIR LPF

// ---------------------------------------------------------------------------
// mic_rec — record 3 s into SRAM, then play back through speaker.
//
// Separates capture and playback into two distinct phases:
//   Phase 1 (capture): only PDM PIO running → CLK uninterrupted, no I2S competition.
//   Phase 2 (playback): only I2S PIO running → clean amp output.
//
// Buffer: REC_SAMPLES × 2 bytes = 1 × 48828 × 2 = 97,656 bytes in BSS.
// ---------------------------------------------------------------------------
#define REC_SECONDS  1
#define REC_SAMPLES  (REC_SECONDS * I2S_SAMPLE_RATE)   // 48,828 int16_t samples

static int16_t rec_buf[REC_SAMPLES];

void mic_loopback(void) {
    printf("\n--- Mic → Speaker Loopback (10 s, Enter to stop) ---\n");
    printf("  PDM 3.125 MHz, decimation=64 → PCM 48828 Hz, gain=×%d\n", MIC_GAIN);

    // --- Start I2S amp on pio0 ---
    int amp_sm = amp_pio_start();
    if (amp_sm < 0) return;

    // Test tone before PDM mic starts — isolates amp vs signal-processing issues.
    // If this tone is heard: amp chain works, problem is in PDM→PCM path.
    // If this tone is also silent: fundamental amp issue specific to mic_loop.
    printf("  Test tone 440 Hz 0.5 s — should be audible...\n");
    bee_note(amp_sm, 440, 500, MAX_AMPLITUDE);
    printf("  Tone done. Starting mic...\n");

    // --- Start PDM mic on pio1 ---
    int mic_sm_num = pio_claim_unused_sm(pio1, false);
    if (mic_sm_num < 0) {
        printf("ERROR: no free PIO1 SM\n");
        amp_pio_stop(amp_sm);
        return;
    }
    uint mic_sm = (uint)mic_sm_num;

    if (!pio_can_add_program(pio1, &pdm_mic_program)) {
        printf("ERROR: PIO1 program space full\n");
        pio_sm_unclaim(pio1, mic_sm);
        amp_pio_stop(amp_sm);
        return;
    }
    uint mic_offset = pio_add_program(pio1, &pdm_mic_program);
    pdm_mic_program_init(pio1, mic_sm, mic_offset, MIC_CLK_PIN, MIC_DATA_PIN, PDM_CLK_DIV);
    pio_sm_set_enabled(pio1, mic_sm, true);

    // Drain any residual serial bytes
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}

    // IIR low-pass filter state: y[n] = (y[n-1]*3 + x[n]) >> 2
    // Cutoff ~2.25 kHz — fc = arccos(23/24) × fs / 2π ≈ 2254 Hz; passes voice, attenuates HF.
    int32_t filtered = 0;

    // DC blocking: very slow IIR HPF, removes DC offset in the PDM path.
    // fc ≈ 7.6 Hz; settles in ~20 ms.
    // dc_est tracks the long-term mean of filtered; subtracting it centres
    // the signal around zero so the speaker receives AC, not a static offset.
    int32_t dc_est = 0;

    // Warm-up: 100 ms — pre-settle filtered and dc_est so the main loop
    // starts with correct DC baseline; output silence to amp during this time.
    uint32_t warmup_end = to_ms_since_boot(get_absolute_time()) + 100;
    while (to_ms_since_boot(get_absolute_time()) < warmup_end) {
        uint32_t w0 = pio_sm_get_blocking(pio1, mic_sm);
        uint32_t w1 = pio_sm_get_blocking(pio1, mic_sm);
        int ones_wu = __builtin_popcount(w0) + __builtin_popcount(w1);
        int32_t pcm_wu = (int32_t)(ones_wu - 32) * 1024 * MIC_GAIN;
        filtered = (filtered * 3 + pcm_wu) >> 2;
        dc_est += (filtered - dc_est) >> 10;
        pio_sm_put_blocking(pio0, (uint)amp_sm, 0);
    }

    printf("  Live — speak into mic...\n");

    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 10000;

#if MIC_LOOP_STATS
    // Debug stats — printed every ~0.5 s (24414 samples).
    // WARNING: printf stalls CPU long enough to freeze PDM CLK → mic loses lock.
    // Keep disabled (MIC_LOOP_STATS=0) when testing audio quality.
    int ones_min = 64, ones_max = 0;
    int32_t filt_min = INT32_MAX, filt_max = INT32_MIN;
    int stat_count = 0;
    const int STAT_INTERVAL = 24414;
#endif

    while (to_ms_since_boot(get_absolute_time()) < deadline) {
        if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) break;

        // 2 PDM words = 64 bits = 1 sample at 48 828 Hz
        uint32_t w0 = pio_sm_get_blocking(pio1, mic_sm);
        uint32_t w1 = pio_sm_get_blocking(pio1, mic_sm);

        // Integrate-and-dump: ones in [0,64], centre 32
        int ones = __builtin_popcount(w0) + __builtin_popcount(w1);
        int32_t pcm = (int32_t)(ones - 32) * 1024 * MIC_GAIN;

        // IIR LPF: α=0.75 — attenuates PDM noise shaping above ~2.25 kHz
        filtered = (filtered * 3 + pcm) >> 2;

        // DC blocking: track and remove the slowly-varying mean of filtered
        dc_est += (filtered - dc_est) >> 10;  // fc ≈ 48828 / (2π × 1024) ≈ 7.6 Hz

        // Clamp to MAX_AMPLITUDE for speaker protection
        int32_t out = filtered - dc_est;
        if (out >  MAX_AMPLITUDE) out =  MAX_AMPLITUDE;
        if (out < -MAX_AMPLITUDE) out = -MAX_AMPLITUDE;
        int16_t s = (int16_t)out;

        // I2S word: bits[31:16]=L, bits[15:0]=R (mono mic → both channels)
        pio_sm_put_blocking(pio0, (uint)amp_sm, ((uint32_t)(uint16_t)s << 16) | (uint16_t)s);

#if MIC_LOOP_STATS
        if (ones < ones_min) ones_min = ones;
        if (ones > ones_max) ones_max = ones;
        if (out < filt_min) filt_min = out;
        if (out > filt_max) filt_max = out;

        if (++stat_count >= STAT_INTERVAL) {
            printf("  ones[%2d..%2d]  out_dc_blocked[%7d..%7d]\n",
                   ones_min, ones_max, (int)filt_min, (int)filt_max);
            ones_min = 16; ones_max = 0;
            filt_min = INT32_MAX; filt_max = INT32_MIN;
            stat_count = 0;
        }
#endif
    }

    // Drain: output silence to avoid click/pop on stop
    for (int i = 0; i < 256; i++)
        pio_sm_put_blocking(pio0, (uint)amp_sm, 0);

    pio_sm_set_enabled(pio1, mic_sm, false);
    pio_remove_program(pio1, &pdm_mic_program, mic_offset);
    pio_sm_unclaim(pio1, mic_sm);
    gpio_set_function(MIC_CLK_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(MIC_DATA_PIN, GPIO_FUNC_NULL);

    amp_pio_stop(amp_sm);
    printf("Done\n");
}

// ---------------------------------------------------------------------------
// mic_rec — record REC_SECONDS into SRAM then play back.
// ---------------------------------------------------------------------------

void mic_rec(void) {
    printf("\n--- Mic Record + Playback (%d s, PDM 3.125 MHz → PCM 48828 Hz) ---\n",
           REC_SECONDS);
    printf("  Buffer: %u samples = %u bytes in SRAM\n",
           (unsigned)REC_SAMPLES, (unsigned)(REC_SAMPLES * sizeof(int16_t)));

    // --- Phase 1: capture — only PDM PIO running, no I2S ---
    int mic_sm_num = pio_claim_unused_sm(pio1, false);
    if (mic_sm_num < 0) { printf("ERROR: no free PIO1 SM\n"); return; }
    uint mic_sm = (uint)mic_sm_num;

    if (!pio_can_add_program(pio1, &pdm_mic_program)) {
        printf("ERROR: PIO1 program space full\n");
        pio_sm_unclaim(pio1, mic_sm);
        return;
    }
    uint mic_offset = pio_add_program(pio1, &pdm_mic_program);
    pdm_mic_program_init(pio1, mic_sm, mic_offset, MIC_CLK_PIN, MIC_DATA_PIN, PDM_CLK_DIV);
    pio_sm_set_enabled(pio1, mic_sm, true);

    // Warm-up: 100 ms to settle mic and IIR/DC filter state
    int32_t filtered = 0, dc_est = 0;
    uint32_t warmup_end = to_ms_since_boot(get_absolute_time()) + 100;
    while (to_ms_since_boot(get_absolute_time()) < warmup_end) {
        uint32_t w0 = pio_sm_get_blocking(pio1, mic_sm);
        uint32_t w1 = pio_sm_get_blocking(pio1, mic_sm);
        int ones = __builtin_popcount(w0) + __builtin_popcount(w1);
        int32_t pcm = (int32_t)(ones - 32) * 1024 * MIC_GAIN;
        filtered = (filtered * 3 + pcm) >> 2;
        dc_est += (filtered - dc_est) >> 10;
    }

    printf("  Recording — speak now...\n");

    for (uint32_t i = 0; i < REC_SAMPLES; i++) {
        uint32_t w0 = pio_sm_get_blocking(pio1, mic_sm);
        uint32_t w1 = pio_sm_get_blocking(pio1, mic_sm);
        int ones = __builtin_popcount(w0) + __builtin_popcount(w1);
        int32_t pcm = (int32_t)(ones - 32) * 1024 * MIC_GAIN;
        filtered = (filtered * 3 + pcm) >> 2;
        dc_est += (filtered - dc_est) >> 10;
        int32_t out = filtered - dc_est;
        if (out >  MAX_AMPLITUDE) out =  MAX_AMPLITUDE;
        if (out < -MAX_AMPLITUDE) out = -MAX_AMPLITUDE;
        rec_buf[i] = (int16_t)out;
    }

    pio_sm_set_enabled(pio1, mic_sm, false);
    pio_remove_program(pio1, &pdm_mic_program, mic_offset);
    pio_sm_unclaim(pio1, mic_sm);
    gpio_set_function(MIC_CLK_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(MIC_DATA_PIN, GPIO_FUNC_NULL);

    // Safe to printf now — PDM CLK is stopped
    printf("  Capture done. Playing back...\n");

    // --- Phase 2: playback — only I2S PIO running, no PDM ---
    int amp_sm = amp_pio_start();
    if (amp_sm < 0) return;

    for (uint32_t i = 0; i < REC_SAMPLES; i++) {
        int16_t s = rec_buf[i];
        pio_sm_put_blocking(pio0, (uint)amp_sm,
                            ((uint32_t)(uint16_t)s << 16) | (uint16_t)s);
    }
    for (int i = 0; i < 256; i++)
        pio_sm_put_blocking(pio0, (uint)amp_sm, 0);

    amp_pio_stop(amp_sm);
    printf("Done\n");
}

// ---------------------------------------------------------------------------
// mic_dump — record 1 s into SRAM then send raw int16 PCM over serial.
//
// Protocol (received by recv_pcm_dump.py):
//   PCMDUMP_START <sample_rate> <num_samples>\n
//   <raw int16_t bytes, little-endian, num_samples × 2 bytes>
//   PCMDUMP_END\n
//
// Transfer time at USB CDC: ~1–2 s for 97 656 bytes (USB full-speed).
// Use recv_pcm_dump.py — do NOT run via bringup_run.ps1 (text-only receiver).
// ---------------------------------------------------------------------------

#define DUMP_SAMPLES  I2S_SAMPLE_RATE   // 1 s = 48 828 samples = 97 656 bytes

void mic_dump(void) {
    printf("\n--- Mic Dump (1 s record → binary PCM over serial) ---\n");
    printf("  Run recv_pcm_dump.py on PC to receive.\n");

    // Phase 1: capture — PDM only
    int mic_sm_num = pio_claim_unused_sm(pio1, false);
    if (mic_sm_num < 0) { printf("ERROR: no free PIO1 SM\n"); return; }
    uint mic_sm = (uint)mic_sm_num;

    if (!pio_can_add_program(pio1, &pdm_mic_program)) {
        printf("ERROR: PIO1 program space full\n");
        pio_sm_unclaim(pio1, mic_sm);
        return;
    }
    uint mic_offset = pio_add_program(pio1, &pdm_mic_program);
    pdm_mic_program_init(pio1, mic_sm, mic_offset, MIC_CLK_PIN, MIC_DATA_PIN, PDM_CLK_DIV);
    pio_sm_set_enabled(pio1, mic_sm, true);

    int32_t filtered = 0, dc_est = 0;
    uint32_t warmup_end = to_ms_since_boot(get_absolute_time()) + 100;
    while (to_ms_since_boot(get_absolute_time()) < warmup_end) {
        uint32_t w0 = pio_sm_get_blocking(pio1, mic_sm);
        uint32_t w1 = pio_sm_get_blocking(pio1, mic_sm);
        int ones = __builtin_popcount(w0) + __builtin_popcount(w1);
        int32_t pcm = (int32_t)(ones - 32) * 1024 * MIC_GAIN;
        filtered = (filtered * 3 + pcm) >> 2;
        dc_est += (filtered - dc_est) >> 10;
    }

    printf("  Recording 1 s — speak now...\n");

    for (uint32_t i = 0; i < DUMP_SAMPLES; i++) {
        uint32_t w0 = pio_sm_get_blocking(pio1, mic_sm);
        uint32_t w1 = pio_sm_get_blocking(pio1, mic_sm);
        int ones = __builtin_popcount(w0) + __builtin_popcount(w1);
        int32_t pcm = (int32_t)(ones - 32) * 1024 * MIC_GAIN;
        filtered = (filtered * 3 + pcm) >> 2;
        dc_est += (filtered - dc_est) >> 10;
        int32_t out = filtered - dc_est;
        if (out >  MAX_AMPLITUDE) out =  MAX_AMPLITUDE;
        if (out < -MAX_AMPLITUDE) out = -MAX_AMPLITUDE;
        rec_buf[i] = (int16_t)out;
    }

    pio_sm_set_enabled(pio1, mic_sm, false);
    pio_remove_program(pio1, &pdm_mic_program, mic_offset);
    pio_sm_unclaim(pio1, mic_sm);
    gpio_set_function(MIC_CLK_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(MIC_DATA_PIN, GPIO_FUNC_NULL);

    // Phase 2: binary dump over serial
    // Flush any buffered text first so PCMDUMP_START arrives before binary data.
    printf("PCMDUMP_START %u %u\n", (unsigned)I2S_SAMPLE_RATE, (unsigned)DUMP_SAMPLES);
    stdio_flush();

    const uint8_t *p = (const uint8_t *)rec_buf;
    for (uint32_t i = 0; i < DUMP_SAMPLES * sizeof(int16_t); i++)
        putchar_raw((int)p[i]);
    stdio_flush();

    printf("PCMDUMP_END\n");
    printf("Done\n");
}

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
    bee_note(sm,G,q,amp); bee_note(sm,E,q,amp); bee_note(sm,E,h,amp);
    bee_note(sm,F,q,amp); bee_note(sm,D,q,amp); bee_note(sm,D,h,amp);

    // 1 3 5 5 | 3---
    bee_note(sm,C,q,amp); bee_note(sm,E,q,amp); bee_note(sm,G,q,amp); bee_note(sm,G,q,amp);
    bee_note(sm,E,w,amp);

    // 2 2 2 2 | 2 3 4-
    bee_note(sm,D,q,amp); bee_note(sm,D,q,amp); bee_note(sm,D,q,amp); bee_note(sm,D,q,amp);
    bee_note(sm,D,q,amp); bee_note(sm,E,q,amp); bee_note(sm,F,h,amp);

    // 3 3 3 3 | 3 4 5-
    bee_note(sm,E,q,amp); bee_note(sm,E,q,amp); bee_note(sm,E,q,amp); bee_note(sm,E,q,amp);
    bee_note(sm,E,q,amp); bee_note(sm,F,q,amp); bee_note(sm,G,h,amp);

    // 5 3 3- | 4 2 2- | 1 3 5 5 | 1---
    bee_note(sm,G,q,amp); bee_note(sm,E,q,amp); bee_note(sm,E,h,amp);
    bee_note(sm,F,q,amp); bee_note(sm,D,q,amp); bee_note(sm,D,h,amp);
    bee_note(sm,C,q,amp); bee_note(sm,E,q,amp); bee_note(sm,G,q,amp); bee_note(sm,G,q,amp);
    bee_note(sm,C,w,amp);

    bee_silence(sm, 50);
    amp_pio_stop(sm);
    printf("Done\n");
}
