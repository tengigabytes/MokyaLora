#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#include "i2s_out.pio.h"

// ---------------------------------------------------------------------------
// Pin / peripheral definitions
// ---------------------------------------------------------------------------

// Both sensor bus (GPIO 34/35) and power bus (GPIO 6/7) map to i2c1 on RP2350.
// They cannot be used simultaneously — use bus_b_init / bus_b_deinit to switch.
#define BUS_A_SDA 34    // Sensor bus  (i2c1 — SDK peripheral name)
#define BUS_A_SCL 35
#define BUS_B_SDA 6     // Power bus   (i2c1 — same peripheral, different GPIOs)
#define BUS_B_SCL 7
#define MTR_PWM_PIN 9   // PWM4_B — drives low-side MOSFET for vibration motor

// NAU8315 I2S amplifier (GPIO 30/31/32)
// VDD = 3.3 V, BTL 8 Ω speaker: P_max = 3.3² / 16 ≈ 0.68 W (speaker limit 0.7 W)
// AMPLITUDE limited to 50 % → P ≈ 0.17 W (safe margin)
// Sample rate: 125 MHz / 40 / 64 cycles = 48 828 Hz
#define AMP_BCLK_PIN   30
#define AMP_LRCK_PIN   31
#define AMP_DAC_PIN    32
#define I2S_SAMPLE_RATE  48828
#define TONE_FREQ_HZ     444    // 48828 / 110 samples ≈ 444 Hz
#define TONE_TABLE_LEN   110
#define MAX_AMPLITUDE    16384  // 50 % of 32767

// LM27965 LED driver (Bus B, 0x36)
// RSET = 8.25 kΩ → full-scale = 30.3 mA/pin
// Bank A (D1A–D5A): TFT backlight, 5 pins → 151.5 mA @ full
// Bank B (D1B, D2B): Keyboard (3 LEDs/pin); D3B (green indicator, 1 LED → limit 40 % = 12.1 mA)
// Bank C (D1C):     Red indicator LED (1 LED → limit 40 % = 12.1 mA)
#define LM27965_ADDR     0x36
#define LM27965_GP       0x10  // ENA[0] ENB[1] ENC[2] EN5A[3] EN3B[4]; default=0x20
#define LM27965_BANKA    0xA0  // bits[4:0]: brightness code Bank A (TFT backlight)
#define LM27965_BANKB    0xB0  // bits[4:0]: brightness code Bank B (Keyboard + D3B)
#define LM27965_BANKC    0xC0  // bits[1:0]: brightness code Bank C (D1C red indicator)
// Brightness codes (Bank A/B): 0x00-0x0F=20%(6mA), 0x10-0x16=40%(12mA),
//                              0x17-0x1C=70%(21mA), 0x1D-0x1F=100%(30mA)
// Brightness codes (Bank C):   bits[1:0] 00=20% 01=40% 10=70% 11=100%

// BQ25622 charger (Bus B, 0x6B)
#define BQ25622_ADDR  0x6B

// BQ25622 register map (SLUSEG2D)
#define REG_CHARGE_CTRL0    0x14
#define REG_CHARGER_CTRL1   0x16  // EN_AUTO_IBATDIS[7] FORCE_IBATDIS[6] EN_CHG[5] EN_HIZ[4] WD_RST[2] WATCHDOG[1:0]
#define REG_CHARGER_CTRL2   0x17
#define REG_CHARGER_CTRL3   0x18  // EN_OTG[6] BATFET_DLY[2] BATFET_CTRL[1:0]
#define REG_CHARGER_CTRL4   0x19
#define REG_NTC_CTRL0       0x1A  // TS_IGNORE[7]
#define REG_STATUS0         0x1D  // VSYS_STAT[4] WD_STAT[0]
#define REG_STATUS1         0x1E  // CHG_STAT[4:3] VBUS_STAT[2:0]
#define REG_FAULT_STATUS    0x1F  // BAT_FAULT_STAT[6] TS_STAT[2:0]
#define REG_CHG_FLAG0       0x20
#define REG_CHG_FLAG1       0x21
#define REG_FAULT_FLAG0     0x22
#define REG_PART_INFO       0x38  // PN[5:3] DEV_REV[2:0]  0x02 = BQ25622

// ---------------------------------------------------------------------------
// LM27965 helpers
// ---------------------------------------------------------------------------

static int lm_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(i2c1, LM27965_ADDR, buf, 2, false, 50000);
}

static int lm_read(uint8_t reg, uint8_t *val) {
    int r = i2c_write_timeout_us(i2c1, LM27965_ADDR, &reg, 1, true, 50000);
    if (r < 0) return r;
    return i2c_read_timeout_us(i2c1, LM27965_ADDR, val, 1, false, 50000);
}

static void lm27965_cycle(void) {
    printf("\n--- LM27965 LED Cycle (non-LCD) ---\n");

    // 40 % brightness = 12.1 mA/pin (safe for all attached LEDs)
    lm_write(LM27965_BANKA, 0x16);
    lm_write(LM27965_BANKB, 0x16);
    lm_write(LM27965_BANKC, 0xFD);  // bits[1:0] = 01 = 40 %

    // GP bit assignments: bit0=ENA(TFT,skip) bit1=ENB(Key D1B+D2B) bit2=ENC(D1C red)
    //                     bit3=EN5A  bit4=EN3B(D3B green)  bit5=reserved(keep 1)
    // ENB(bit1) must be set for any Bank B output (required for D3B green too).
    // EN3B(bit4) gates only D3B; D1B+D2B are on whenever ENB=1.
    typedef struct { uint8_t gp; const char *name; } step_t;
    step_t seq[] = {
        { 0x22, "Keyboard backlight (D1B+D2B)"          },
        { 0x24, "Red indicator (D1C)"                   },
        { 0x32, "Green indicator (D3B) + Keyboard"      },
        { 0x36, "All: Keyboard + Red + Green"           },
    };

    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < 4; i++) {
            lm_write(LM27965_GP, seq[i].gp);
            printf("  [%d] %s\n", cycle, seq[i].name);
            sleep_ms(600);
        }
        lm_write(LM27965_GP, 0x20);  // brief off
        sleep_ms(200);
    }

    lm_write(LM27965_GP, 0x20);
    printf("Done - LEDs off\n");
}

// ---------------------------------------------------------------------------
// Power bus (Bus B) init / deinit
// ---------------------------------------------------------------------------

static void bus_b_init(void) {
    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_B_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_B_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_B_SDA);
    gpio_pull_up(BUS_B_SCL);
}

static void bus_b_deinit(void) {
    i2c_deinit(i2c1);
    gpio_set_function(BUS_B_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_B_SCL, GPIO_FUNC_NULL);
}

// ---------------------------------------------------------------------------
// BQ25622 helpers
// ---------------------------------------------------------------------------

static int bq_read(uint8_t reg, uint8_t *val) {
    int r = i2c_write_timeout_us(i2c1, BQ25622_ADDR, &reg, 1, true, 50000);
    if (r < 0) return r;
    return i2c_read_timeout_us(i2c1, BQ25622_ADDR, val, 1, false, 50000);
}

static int bq_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(i2c1, BQ25622_ADDR, buf, 2, false, 50000);
}

static void bq25622_print_status(void) {
    uint8_t v;
    printf("\n--- BQ25622 Status ---\n");

    if (bq_read(REG_PART_INFO, &v) >= 0) {
        uint8_t pn = (v >> 3) & 0x7;
        uint8_t rev = v & 0x7;
        printf("Part Info  (0x38): 0x%02X  PN=%s  REV=%d\n",
               v, pn == 1 ? "BQ25622" : (pn == 0 ? "BQ25620" : "???"), rev);
    }
    if (bq_read(REG_STATUS0, &v) >= 0) {
        printf("Status0    (0x1D): 0x%02X  VSYS_STAT=%d(%s)  WD_STAT=%d(%s)\n",
               v,
               (v >> 4) & 1, (v >> 4) & 1 ? "BAT<VSYSMIN" : "OK",
               v & 1,        v & 1         ? "WD expired!" : "OK");
    }
    if (bq_read(REG_STATUS1, &v) >= 0) {
        uint8_t chg  = (v >> 3) & 0x3;
        uint8_t vbus = v & 0x7;
        const char *chg_str[]  = {"Not Charging", "Pre/Fast Charge", "Taper(CV)", "Top-off"};
        const char *vbus_str[] = {"No VBUS", "SDP", "CDP", "DCP",
                                   "Unknown Adapter", "Non-Std", "HVDCP", "OTG"};
        printf("Status1    (0x1E): 0x%02X  CHG=%s  VBUS=%s\n",
               v, chg_str[chg], vbus_str[vbus]);
    }
    if (bq_read(REG_FAULT_STATUS, &v) >= 0) {
        uint8_t ts = v & 0x7;
        const char *ts_str[] = {"NORMAL", "COLD/OTG_COLD/no-bias", "HOT/OTG_HOT", "COOL",
                                 "WARM", "PRECOOL", "PREWARM", "TS-bias-fault"};
        printf("Fault      (0x1F): 0x%02X  BAT_FAULT=%d  SYS_FAULT=%d  TSHUT=%d  TS=%s\n",
               v, (v >> 6) & 1, (v >> 5) & 1, (v >> 3) & 1, ts_str[ts]);
    }
    if (bq_read(REG_CHARGER_CTRL1, &v) >= 0) {
        printf("ChgCtrl1   (0x16): 0x%02X  EN_CHG=%d  EN_HIZ=%d  WATCHDOG=%d\n",
               v, (v >> 5) & 1, (v >> 4) & 1, v & 0x3);
    }
    if (bq_read(REG_NTC_CTRL0, &v) >= 0) {
        printf("NTC_Ctrl0  (0x1A): 0x%02X  TS_IGNORE=%d\n", v, (v >> 7) & 1);
    }
    if (bq_read(REG_CHARGER_CTRL3, &v) >= 0) {
        printf("ChgCtrl3   (0x18): 0x%02X  BATFET_CTRL=%d(%s)  BATFET_DLY=%d\n",
               v, v & 0x3,
               (v & 0x3) == 0 ? "Normal" :
               (v & 0x3) == 1 ? "Shutdown" :
               (v & 0x3) == 2 ? "Ship" : "SysRst",
               (v >> 2) & 1);
    }
    if (bq_read(REG_FAULT_FLAG0, &v) >= 0) {
        printf("FaultFlag  (0x22): 0x%02X\n", v);
    }
}

static void bq25622_disable_charge(void) {
    uint8_t v;
    printf("\n--- BQ25622: Disabling Charge ---\n");
    bq_write(REG_CHARGER_CTRL1, 0x84);  // kick WD_RST first
    sleep_ms(10);
    bq_write(REG_CHARGER_CTRL1, 0x80);  // EN_CHG=0, WATCHDOG disabled
    bq_read(REG_CHARGER_CTRL1, &v);
    printf("ChgCtrl1 (0x16): 0x%02X  EN_CHG=%d  EN_HIZ=%d  WATCHDOG=%d\n",
           v, (v >> 5) & 1, (v >> 4) & 1, v & 0x3);
}

static void bq25622_enable_charge(void) {
    uint8_t v;
    printf("\n--- BQ25622: Enabling Charge ---\n");
    bq_write(REG_CHARGER_CTRL1, 0xA4);  // kick WD_RST, EN_CHG=1, WATCHDOG=01
    sleep_ms(10);
    bq_write(REG_CHARGER_CTRL1, 0xA1);  // EN_CHG=1, WATCHDOG=01 (default)
    bq_read(REG_CHARGER_CTRL1, &v);
    printf("ChgCtrl1 (0x16): 0x%02X  EN_CHG=%d  EN_HIZ=%d  WATCHDOG=%d\n",
           v, (v >> 5) & 1, (v >> 4) & 1, v & 0x3);
}

// ---------------------------------------------------------------------------
// Vibration motor — breathing PWM
// ---------------------------------------------------------------------------

static void motor_breathe(void) {
    printf("\n--- Motor Breathe (GPIO%d, PWM4_B) ---\n", MTR_PWM_PIN);

    gpio_set_function(MTR_PWM_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(MTR_PWM_PIN);
    uint chan  = pwm_gpio_to_channel(MTR_PWM_PIN);

    // 1 kHz PWM: 125 MHz / 125 / 1000 = 1000 Hz
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 125.0f);
    pwm_config_set_wrap(&cfg, 999);
    pwm_init(slice, &cfg, true);
    pwm_set_chan_level(slice, chan, 0);

    for (int b = 0; b < 5; b++) {
        for (int d = 0; d <= 1000; d += 20) {
            pwm_set_chan_level(slice, chan, d > 999 ? 999 : d);
            sleep_ms(20);
        }
        for (int d = 999; d >= 0; d -= 20) {
            pwm_set_chan_level(slice, chan, d < 0 ? 0 : d);
            sleep_ms(20);
        }
        printf("  breath %d done\n", b + 1);
    }

    pwm_set_chan_level(slice, chan, 0);
    pwm_set_enabled(slice, false);
    gpio_set_function(MTR_PWM_PIN, GPIO_FUNC_NULL);
    printf("Motor off\n");
}

// ---------------------------------------------------------------------------
// NAU8315 I2S audio via PIO
// ---------------------------------------------------------------------------

// Unit sine table (full scale) — computed once at startup using hardware FPU
static int16_t sine_unit[TONE_TABLE_LEN];

static void precompute_sine(void) {
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
static void amp_test(void) {
    // 80 % amplitude: P = 0.68 × 0.64 ≈ 0.44 W — under 0.7 W speaker limit
    const int amp = (int)(MAX_AMPLITUDE * 1.6f);
    printf("\n--- NAU8315 Tone Test (%d Hz, amp 80%%, ~0.44 W) ---\n", TONE_FREQ_HZ);
    printf("    Short TP46 to GND if silent (GAIN pin floating)\n");

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
static void amp_breathe(void) {
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

// ---------------------------------------------------------------------------
// I2C bus scan
// ---------------------------------------------------------------------------

void perform_scan(i2c_inst_t *i2c, uint sda, uint scl, const char *bus_name) {
    printf("\n--- Scanning %s (SDA:%d, SCL:%d) ---\n", bus_name, sda, scl);

    i2c_init(i2c, 100 * 1000);
    gpio_set_function(sda, GPIO_FUNC_I2C);
    gpio_set_function(scl, GPIO_FUNC_I2C);
    gpio_pull_up(sda);
    gpio_pull_up(scl);

    printf("   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (int addr = 0; addr < (1 << 7); ++addr) {
        if (addr % 16 == 0) printf("%02x ", addr);
        int ret;
        uint8_t rxdata;
        if ((addr & 0x78) == 0 || (addr & 0x78) == 0x78)
            ret = PICO_ERROR_GENERIC;
        else
            ret = i2c_read_timeout_us(i2c, addr, &rxdata, 1, false, 50000);
        printf(ret < 0 ? "." : "@");
        printf(addr % 16 == 15 ? "\n" : "  ");
    }

    i2c_deinit(i2c);
    gpio_set_function(sda, GPIO_FUNC_NULL);
    gpio_set_function(scl, GPIO_FUNC_NULL);
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

static void handle_command(const char *cmd) {
    if (strcmp(cmd, "scan_a") == 0) {
        perform_scan(i2c1, BUS_A_SDA, BUS_A_SCL, "Bus A (Sensors, i2c1)");
        printf("Expected: 0x6A(IMU)  0x1E(Mag)  0x5D(Baro)  0x3A(GNSS)\n");

    } else if (strcmp(cmd, "scan_b") == 0) {
        perform_scan(i2c1, BUS_B_SDA, BUS_B_SCL, "Bus B (Power, i2c1)");
        printf("Expected: 0x6B(Charger)  0x55(FuelGauge)  0x36(LED)\n");

    } else if (strcmp(cmd, "status") == 0) {
        bus_b_init();
        bq25622_print_status();
        bus_b_deinit();

    } else if (strcmp(cmd, "led") == 0) {
        bus_b_init();
        lm27965_cycle();
        bus_b_deinit();

    } else if (strcmp(cmd, "motor") == 0) {
        motor_breathe();

    } else if (strcmp(cmd, "amp_test") == 0) {
        amp_test();

    } else if (strcmp(cmd, "amp") == 0) {
        amp_breathe();

    } else if (strcmp(cmd, "charge_on") == 0) {
        bus_b_init();
        bq25622_enable_charge();
        bus_b_deinit();

    } else if (strcmp(cmd, "charge_off") == 0) {
        bus_b_init();
        bq25622_disable_charge();
        bus_b_deinit();

    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        printf("Commands:\n");
        printf("  scan_a      -- scan Bus A (sensors, GPIO 34/35)\n");
        printf("  scan_b      -- scan Bus B (power, GPIO 6/7)\n");
        printf("  status      -- BQ25622 charger status\n");
        printf("  led         -- LED cycle (keyboard/red/green)\n");
        printf("  motor       -- vibration motor breathe x5\n");
        printf("  amp_test    -- NAU8315 constant tone 5 s at 80%% (hardware check)\n");
        printf("  amp         -- NAU8315 speaker breathe tone x5 (~444 Hz)\n");
        printf("  charge_on   -- enable BQ25622 charging\n");
        printf("  charge_off  -- disable BQ25622 charging\n");

    } else if (cmd[0] != '\0') {
        printf("Unknown command: '%s'  (type 'help' for list)\n", cmd);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    stdio_init_all();
    sleep_ms(2000);  // wait for power rails to settle

    // Pre-compute unit sine table using hardware FPU sinf()
    precompute_sine();

    // Safe defaults: charge off, LEDs off
    bus_b_init();
    bq25622_disable_charge();
    lm_write(LM27965_GP, 0x20);  // all outputs off
    bus_b_deinit();

    printf("\n\n***************************************\n");
    printf("* MokyaLora RP2350 Bring-Up Shell     *\n");
    printf("***************************************\n");
    printf("Type 'help' for commands.\n> ");

    char line[32];
    int pos = 0;

    while (true) {
        int c = getchar_timeout_us(10000);
        if (c == PICO_ERROR_TIMEOUT) continue;

        if (c == '\r' || c == '\n') {
            line[pos] = '\0';
            pos = 0;
            printf("\n");
            handle_command(line);
            printf("> ");
        } else if (c == '\b' || c == 127) {
            if (pos > 0) { pos--; printf("\b \b"); }
        } else if (pos < (int)(sizeof(line) - 1)) {
            line[pos++] = (char)c;
            printf("%c", c);
        }
    }
}
