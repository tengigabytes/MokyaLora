/* st7789vi.c — ST7789VI 240×320 IPS init sequence.
 *
 * Sequence reproduces the Rev A bringup-validated path (Step 4) with the TEON
 * fix from Step 13: TE pin is gated low after POR until command 0x35 enables
 * V-blank pulses, so the standard init must run TEON before LVGL relies on TE.
 *
 * Register / parameter values cross-checked against ST7789VI datasheet V1.3
 * section 9.1 (User Commands) and the Sitronix application note recommended
 * gamma curve for IPS panels.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "st7789vi.h"

#include "pico/time.h"

void st7789_init(st7789_send_cmd_fn send_cmd, st7789_send_data_fn send_data)
{
    /* Software reset: returns all user registers to POR defaults.
     * Datasheet V1.3 §9.1.1: caller must wait ≥120 ms before sending SLPOUT;
     * 150 ms gives a comfortable margin for the on-chip oscillator to settle. */
    send_cmd(ST7789_SWRESET);
    sleep_ms(150);

    /* Sleep out: powers up booster, regulators, VCOM. Datasheet §9.1.11
     * mandates a 120 ms pause before any subsequent command. */
    send_cmd(ST7789_SLPOUT);
    sleep_ms(120);

    /* COLMOD = 0x55 → DPI[6:4]=101 (16-bit/pixel RGB565) and
     *               DBI[2:0]=101 (16-bit MCU interface). */
    send_cmd(ST7789_COLMOD); send_data(0x55);

    /* MADCTL = 0x00 → MY=MX=MV=ML=RGB=MH=0 → portrait, top-left origin,
     * RGB colour order, no row/column flip. */
    send_cmd(ST7789_MADCTL); send_data(0x00);

    /* Porch: BPA=0x0C, FPA=0x0C, PSEN=0, BPB/FPB=0x33, BPC/FPC=0x33.
     * Datasheet recommended defaults — eliminates wrap-around tearing. */
    send_cmd(ST7789_PORCTRL);
    send_data(0x0C); send_data(0x0C); send_data(0x00);
    send_data(0x33); send_data(0x33);

    /* Gate control: VGH≈13.26 V / VGL≈-10.43 V (typical IPS recommendation). */
    send_cmd(ST7789_GCTRL); send_data(0x35);

    /* VCOM = 0x19 → 0.725 V (centre of the panel sweet spot). */
    send_cmd(ST7789_VCOMS); send_data(0x19);

    /* LCM control: XMH=1, XBGR=0, XINV=1, XMX=0, XMV=0, XMY=0 — reflects the
     * MADCTL default but exposed via the LCM register so reset paths agree. */
    send_cmd(ST7789_LCMCTRL); send_data(0x2C);

    /* VDV/VRH come from registers (not OTP). */
    send_cmd(ST7789_VDVVRHEN); send_data(0x01);

    /* VRH = 0x12 → VAP/VAN ≈ ±4.45 V (matches IPS gamma table). */
    send_cmd(ST7789_VRHS); send_data(0x12);

    /* VDV = 0x20 (POR default; explicit for clarity). */
    send_cmd(ST7789_VDVS); send_data(0x20);

    /* Frame rate ctrl 2: NLA=000 (60 Hz in normal mode), RTNA=0x0F. */
    send_cmd(ST7789_FRCTRL2); send_data(0x0F);

    /* Power control 1: AVDD=6.8 V, AVCL=-4.8 V, VDS=2.3 V. */
    send_cmd(ST7789_PWCTRL1); send_data(0xA4); send_data(0xA1);

    /* Positive gamma — Sitronix IPS reference table. */
    send_cmd(ST7789_PVGAMCTRL);
    send_data(0xD0); send_data(0x04); send_data(0x0D); send_data(0x11);
    send_data(0x13); send_data(0x2B); send_data(0x3F); send_data(0x54);
    send_data(0x4C); send_data(0x18); send_data(0x0D); send_data(0x0B);
    send_data(0x1F); send_data(0x23);

    /* Negative gamma — Sitronix IPS reference table. */
    send_cmd(ST7789_NVGAMCTRL);
    send_data(0xD0); send_data(0x04); send_data(0x0C); send_data(0x11);
    send_data(0x13); send_data(0x2C); send_data(0x3F); send_data(0x44);
    send_data(0x51); send_data(0x2F); send_data(0x1F); send_data(0x1F);
    send_data(0x20); send_data(0x23);

    /* Default address window: full 240×320. */
    send_cmd(ST7789_CASET);
    send_data(0x00); send_data(0x00);
    send_data(0x00); send_data(0xEF);   /* 239 */
    send_cmd(ST7789_RASET);
    send_data(0x00); send_data(0x00);
    send_data(0x01); send_data(0x3F);   /* 319 */

    /* IPS panels need inversion ON to render correct colours. */
    send_cmd(ST7789_INVON);

    /* Tearing effect: mode = 0 → output only at V-blank rising edge.
     * Without this command TE stays low forever. */
    send_cmd(ST7789_TEON); send_data(0x00);

    /* Display ON. Datasheet recommends ≥10 ms before first RAMWR; 20 ms
     * matches the bringup sequence. */
    send_cmd(ST7789_DISPON);
    sleep_ms(20);
}
