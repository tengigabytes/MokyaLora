#pragma once

// NAU8315 I2S amplifier PIO lifecycle (used by bringup_mic.c for loopback)
int  amp_pio_start(void);
void amp_pio_stop(int sm);
void bee_note(int sm, int freq, int dur_ms, int amp);
