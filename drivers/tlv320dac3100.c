/*
 * frank-micro — BBC Micro for RP2350
 *
 * Copyright (c) 2026 Andrew de Quincey <adq@lidskialf.net>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * tlv320dac3100.c — TI TLV320DAC3100 audio codec bring-up over I2C.
 *
 * The register sequence is adapted from adafruit/pico-mac at commit 59c910b,
 * src/main.c:580-745, which is a working bring-up of this codec on this board.
 * adafruit/pico-mac is a fork of Matt Evans's pico-umac, and that file carries
 * his notice, reproduced here because MIT requires it be preserved in
 * derivative works:
 *
 *     Copyright 2024 Matt Evans
 *     SPDX-License-Identifier: MIT
 *
 * MIT is compatible with this file's GPL-3.0-or-later.  Three deliberate
 * departures from the borrowed sequence are marked DEPARTURE below.
 *
 * The clock constants are NOT from that project.  They come from
 * tools/tlv320_clocks.py; see the header for why the borrowed ones are wrong.
 *
 * Register map, from TI document SLAS671C.  The part has two register pages and
 * page 0 is selected by writing 0 to register 0, which is why every block below
 * says which page it is on.  Getting that wrong writes a plausible value to
 * completely the wrong register, so the page is set explicitly rather than
 * tracked.
 */

#include "tlv320dac3100.h"
#include "board_config.h"

#include <stdio.h>

/* The whole driver reduces to no-ops on a board with no codec. */
#ifdef CODEC_I2C_ADDR

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

/* ── register addresses ───────────────────────────────────────────────────
 * Named rather than inlined, because a bare 0x2A appearing on two different
 * pages is how this driver would get silently wrong. */

/* page 0 */
#define REG_PAGE_SELECT      0x00
#define REG_SOFT_RESET       0x01
#define REG_CLOCK_MUX        0x04   /* PLL_CLKIN and CODEC_CLKIN sources    */
#define REG_PLL_P_R          0x05   /* bit7 power, [6:4] P, [3:0] R         */
#define REG_PLL_J            0x06
#define REG_PLL_D_MSB        0x07
#define REG_PLL_D_LSB        0x08
#define REG_NDAC             0x0B   /* bit7 power, [6:0] divider            */
#define REG_MDAC             0x0C   /* bit7 power, [6:0] divider            */
#define REG_DOSR_MSB         0x0D
#define REG_DOSR_LSB         0x0E   /* must follow the MSB immediately      */
#define REG_AUDIO_IFACE      0x1B   /* [7:6] format, [5:4] word length      */
#define REG_DAC_DATAPATH     0x3F   /* [7:6] power up left and right DAC    */
#define REG_DAC_MUTE         0x40   /* [3:2] mute left and right            */
#define REG_DAC_VOL_LEFT     0x41
#define REG_DAC_VOL_RIGHT    0x42
#define REG_HEADSET_DETECT    0x43  /* bit7 enable                          */

/* page 1 */
#define P1_HP_DRIVER         0x1F   /* [7:6] power headphone drivers        */
#define P1_SPK_AMP           0x20   /* bit7 enable class-D amplifier        */
#define P1_OUTPUT_ROUTING    0x23
#define P1_HP_ANA_VOL_LEFT   0x24
#define P1_HP_ANA_VOL_RIGHT  0x25
#define P1_SPK_ANA_VOL       0x26
#define P1_HPL_DRIVER        0x28   /* bit2 unmuted, [6:3] gain             */
#define P1_HPR_DRIVER        0x29   /* bit2 unmuted, [6:3] gain             */
#define P1_SPK_DRIVER        0x2A   /* bit2 unmuted, [4:3] gain             */
#define P1_HEADSET_DEBOUNCE  0x2E

#define UNMUTE_BIT           0x04   /* bit2 in each of the three drivers    */

/*
 * Analogue output level.  The two headphone registers and the speaker register
 * all count *down* in roughly 0.5 dB steps, so 0 is the codec's 0 dB and larger
 * values are quieter.
 *
 * This is driven from the F12 volume setting rather than fixed, so attenuation
 * happens here in the analogue domain instead of by dividing the samples. The
 * digital path then keeps all sixteen bits. 100 percent maps to 0, full output.
 *
 * pico-mac hardcodes 50 for the headphones and 40 for the speaker, roughly -26
 * and -20 dB. That was the starting point here, and it is audible but distinctly
 * quiet even with the software volume at maximum.
 *
 * ATTEN_AT_ZERO is where 0 percent lands. It is a deep attenuation rather than
 * the register maximum: -60 dB is inaudible in practice, and stopping short of
 * the bottom keeps the mapping linear across the whole useful range.
 */
#define ATTEN_AT_ZERO        120    /* about -60 dB at 0 percent */

#define I2C_TIMEOUT_US       2000

static bool s_present = false;
static bool s_muted   = true;   /* amps come up muted; unmuted on first audio */
static int  s_volume  = -1;     /* last applied percent; -1 = never set */

/* ── I2C primitives ───────────────────────────────────────────────────────
 * Every one of these can fail, and none of them is worth halting the machine
 * for, so failures set s_present false and the caller gives up quietly.  This
 * is the one substantive difference from the borrowed code, which panics. */

static bool reg_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    int res = i2c_write_timeout_us(i2c0, CODEC_I2C_ADDR, buf, 2, false,
                                  I2C_TIMEOUT_US);
    if (res != 2) {
        s_present = false;
        return false;
    }
    return true;
}

static bool reg_read(uint8_t reg, uint8_t *out) {
    uint8_t buf[1] = { reg };
    if (i2c_write_timeout_us(i2c0, CODEC_I2C_ADDR, buf, 1, true,
                             I2C_TIMEOUT_US) != 1) {
        s_present = false;
        return false;
    }
    if (i2c_read_timeout_us(i2c0, CODEC_I2C_ADDR, buf, 1, false,
                            I2C_TIMEOUT_US) != 1) {
        s_present = false;
        return false;
    }
    *out = buf[0];
    return true;
}

/* Read-modify-write, so a write touches only the bits it owns.  Several of
 * these registers hold unrelated settings in their other bits. */
static bool reg_modify(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t current;
    if (!reg_read(reg, &current)) return false;
    return reg_write(reg, (uint8_t)((current & (uint8_t)~mask) | (value & mask)));
}

static bool set_page(uint8_t page) {
    return reg_write(REG_PAGE_SELECT, page);
}

/* ── clock configuration ──────────────────────────────────────────────────
 *
 * From tools/tlv320_clocks.py.  Valid at both 31250 and 32000 Hz, because the
 * PLL is fed from BCLK and BCLK is 32 times the sample rate, so the whole
 * divider chain scales with the rate and the ratios stay exact:
 *
 *   NDAC x MDAC x DOSR == 32 x R x J / P == 3328
 *
 * At 31250 Hz that puts the PLL at 104.000 MHz and DAC_MOD_CLK at 4.000 MHz,
 * both comfortably inside their specified ranges.  Run the tool, with
 * --self-test, before changing any of these.
 */
#define PLL_P     1
#define PLL_R     2
#define PLL_J     52
#define DIV_NDAC  13
#define DIV_MDAC  2
#define DIV_DOSR  128

static bool configure_clocks(void) {
    if (!set_page(0)) return false;

    /* PLL_CLKIN = BCLK, CODEC_CLKIN = PLL_CLK.  No MCLK is driven on this
     * board and none is needed. */
    if (!reg_modify(REG_CLOCK_MUX, 0x03, 0x03)) return false;
    if (!reg_modify(REG_CLOCK_MUX, 0x0C, 0x04)) return false;

    /* J, then D as two writes.  D stays zero, which is what allows R to be
     * greater than 1. */
    if (!reg_write(REG_PLL_J,     PLL_J)) return false;
    if (!reg_write(REG_PLL_D_MSB, 0x00))  return false;
    if (!reg_write(REG_PLL_D_LSB, 0x00))  return false;

    /* P and R with the PLL still powered down: the multiplier has to be valid
     * before the PLL starts. */
    if (!reg_modify(REG_PLL_P_R, 0x0F, PLL_R)) return false;
    if (!reg_modify(REG_PLL_P_R, 0x70, (uint8_t)(PLL_P << 4))) return false;

    /* DAC dividers, each with its power bit. */
    if (!reg_modify(REG_NDAC, 0x7F, DIV_NDAC)) return false;
    if (!reg_modify(REG_NDAC, 0x80, 0x80))     return false;
    if (!reg_modify(REG_MDAC, 0x7F, DIV_MDAC)) return false;
    if (!reg_modify(REG_MDAC, 0x80, 0x80))     return false;

    /* DOSR is 16 bits and the LSB write is what commits it, so these two must
     * stay adjacent and in this order. */
    if (!reg_write(REG_DOSR_MSB, (uint8_t)(DIV_DOSR >> 8)))   return false;
    if (!reg_write(REG_DOSR_LSB, (uint8_t)(DIV_DOSR & 0xFF))) return false;

    /* DEPARTURE from pico-mac: it also writes NADC and MADC at 0x12 and 0x13.
     * This part is a DAC and has no ADC, so there is nothing to divide. */

    /* Power the PLL up and give it the datasheet's start-up time. */
    if (!reg_modify(REG_PLL_P_R, 0x80, 0x80)) return false;
    sleep_ms(10);

    return true;
}

/* ── public API ───────────────────────────────────────────────────────── */

bool tlv320_init(uint32_t sample_rate_hz) {
    /* The sample rate does not appear in any register: the PLL runs from BCLK,
     * so the rate is set entirely by what the I2S driver clocks out, and the
     * divider chain above is exact at both rates frank-micro can produce.  The
     * argument is here to make that explicit at the call site and to catch a
     * rate the constants were never checked against. */
    if (sample_rate_hz != 31250 && sample_rate_hz != 32000) {
        printf("codec: %lu Hz is not a rate the dividers were solved for; "
               "re-run tools/tlv320_clocks.py\n", (unsigned long)sample_rate_hz);
        return false;
    }

    /* PERIPH_RST has a 10K pull-up, so the codec and the ESP32-C6 are both out
     * of reset already.  Driving it high is a no-op that makes the state
     * explicit; driving it LOW would reset the ESP32 too, so never do that. */
    gpio_init(PERIPH_RESET_PIN);
    gpio_set_dir(PERIPH_RESET_PIN, GPIO_OUT);
    gpio_put(PERIPH_RESET_PIN, 1);

    /* I2C0 at 100 kHz.  This bus is shared with the STEMMA QT connector and the
     * 2x16 header, so anything the user plugs in is on it too. */
    i2c_init(i2c0, 100 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    s_present = true;   /* cleared by the first failing transfer */
    s_muted   = true;

    /* Software reset, then the datasheet's internal-initialisation time.
     * DEPARTURE from pico-mac, which sleeps a full second here for no reason
     * the datasheet gives. */
    if (!set_page(0))                       goto fail;
    if (!reg_write(REG_SOFT_RESET, 0x01))   goto fail;
    sleep_ms(2);

    /* I2S format, 16-bit words.  Our I2S driver clocks 32 BCLKs per stereo
     * frame, which is two 16-bit channels. */
    if (!reg_modify(REG_AUDIO_IFACE, 0xC0, 0x00)) goto fail;
    if (!reg_modify(REG_AUDIO_IFACE, 0x30, 0x00)) goto fail;

    if (!configure_clocks()) goto fail;

    /* Headphone detect is the codec's own VOL/MICDET pin on the jack's ring,
     * not a GPIO, which is why it is a register and not a pin read. */
    if (!set_page(1))                                  goto fail;
    if (!reg_modify(P1_HEADSET_DEBOUNCE, 0xFF, 0x0B))   goto fail;
    if (!set_page(0))                                  goto fail;
    if (!reg_modify(REG_HEADSET_DETECT, 0x80, 0x80))    goto fail;

    /* DEPARTURE from pico-mac: it also writes 0x30 (INT1 routing) and 0x33,
     * which configures the codec's GPIO1 as an output.  On this board GPIO 23
     * is a three-way net joining that pin to the ESP32-C6's IO9/BOOT9 and to
     * the ESP_BOOT header, and the ESP32 is out of reset by default, so driving
     * it can put the ESP32 into download mode.  frank-micro has no use for
     * codec GPIO1 and does not service its interrupt, so both writes are
     * omitted.  Do not add them back without deciding about the ESP32. */

    /* Power both DAC channels and unmute the digital path at 0 dB.  The
     * analogue amps stay muted until there is audio; see tlv320_set_muted. */
    if (!reg_modify(REG_DAC_DATAPATH, 0xC0, 0xC0)) goto fail;
    if (!reg_modify(REG_DAC_MUTE, 0x0C, 0x00))     goto fail;
    if (!reg_write(REG_DAC_VOL_LEFT,  0x00))       goto fail;
    if (!reg_write(REG_DAC_VOL_RIGHT, 0x00))       goto fail;

    /* Route the DACs to the output drivers. */
    if (!set_page(1))                                goto fail;
    if (!reg_modify(P1_OUTPUT_ROUTING, 0xC0, 0x40))  goto fail;
    if (!reg_modify(P1_OUTPUT_ROUTING, 0x0C, 0x04))  goto fail;

    /* Headphone drivers: powered, 0 dB driver gain. */
    if (!reg_modify(P1_HP_DRIVER, 0xC0, 0xC0))                goto fail;
    if (!reg_modify(P1_HPL_DRIVER, 0x78, 0x00))               goto fail;
    if (!reg_modify(P1_HPR_DRIVER, 0x78, 0x00))               goto fail;

    /* Class-D speaker amplifier: enabled, 0 dB driver gain. */
    if (!reg_modify(P1_SPK_AMP, 0x80, 0x80))            goto fail;
    if (!reg_modify(P1_SPK_DRIVER, 0x18, 0x08))         goto fail;

    if (!set_page(0)) goto fail;

    /* Analogue levels. Starts at full output; frank_audio.c then drives this
     * from the F12 volume setting on every buffer. */
    s_volume = -1;
    tlv320_set_volume(100);

    printf("codec: TLV320DAC3100 ready at 0x%02X (%lu Hz, PLL J=%d NDAC=%d)\n",
           CODEC_I2C_ADDR, (unsigned long)sample_rate_hz, PLL_J, DIV_NDAC);
    return true;

fail:
    printf("codec: TLV320DAC3100 did not respond at 0x%02X; "
           "headphone jack and speaker will be silent\n", CODEC_I2C_ADDR);
    s_present = false;
    return false;
}

void tlv320_bclk_started(void) {
    if (!s_present) return;

    /* The PLL is clocked from BCLK, so if it was powered up before anything
     * was driving the bit clock it has nothing to lock to.  Cycle its power
     * now that BCLK is live and give it the datasheet's start-up time. */
    if (!set_page(0)) return;
    reg_modify(REG_PLL_P_R, 0x80, 0x00);
    sleep_ms(1);
    reg_modify(REG_PLL_P_R, 0x80, 0x80);
    sleep_ms(10);
}

void tlv320_set_volume(int percent) {
    if (!s_present) return;
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    if (percent == s_volume) return;
    s_volume = percent;

    /* Linear in the register, which is linear in dB, so also roughly linear in
     * perceived loudness. 100 percent gives 0, the codec's 0 dB. */
    int atten = ((100 - percent) * ATTEN_AT_ZERO) / 100;
    if (atten > 127) atten = 127;

    if (!set_page(1)) return;
    /* Bit 7 of each of these is left clear, matching the configuration known to
     * work on this board; the routing itself is set by P1_OUTPUT_ROUTING. */
    reg_write(P1_HP_ANA_VOL_LEFT,  (uint8_t)atten);
    reg_write(P1_HP_ANA_VOL_RIGHT, (uint8_t)atten);
    reg_write(P1_SPK_ANA_VOL,      (uint8_t)atten);
    set_page(0);
}

void tlv320_set_muted(bool muted) {
    if (!s_present || muted == s_muted) return;
    s_muted = muted;

    uint8_t bits = muted ? 0x00 : UNMUTE_BIT;
    if (!set_page(1)) return;
    reg_modify(P1_HPL_DRIVER, UNMUTE_BIT, bits);
    reg_modify(P1_HPR_DRIVER, UNMUTE_BIT, bits);
    reg_modify(P1_SPK_DRIVER, UNMUTE_BIT, bits);
    set_page(0);
}

bool tlv320_present(void) {
    return s_present;
}

#else  /* !CODEC_I2C_ADDR — no codec on this board */

bool tlv320_init(uint32_t sample_rate_hz) { (void)sample_rate_hz; return false; }
void tlv320_bclk_started(void)            { }
void tlv320_set_volume(int percent)       { (void)percent; }
void tlv320_set_muted(bool muted)         { (void)muted; }
bool tlv320_present(void)                 { return false; }

#endif /* CODEC_I2C_ADDR */
