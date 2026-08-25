/*
 * frank-micro — BBC Micro for RP2350
 *
 * Copyright (c) 2026 Andrew de Quincey <adq@lidskialf.net>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * tlv320dac3100.h — TI TLV320DAC3100 audio codec, I2C control.
 *
 * The codec is the only audio output path on the Adafruit Fruit Jam: the 3.5 mm
 * headphone jack and the onboard mono speaker connector are both downstream of
 * it, and it is silent until configured.  It takes I2S audio and is controlled
 * over I2C at address 0x18.
 *
 * It does nothing on a board that does not declare CODEC_I2C_ADDR; every
 * function below then compiles to a no-op, so callers need no guard.
 *
 * The clock constants come from tools/tlv320_clocks.py, which derives them from
 * the constraints in TI document SLAS671C and prints the working.  Do not
 * hand-edit them, and do not copy them from another project: the register block
 * that appears in both adafruit/pico-mac and adafruit/fruitjam-doom encodes a
 * valid *ratio* but puts the PLL or DAC_MOD_CLK outside its specified range at
 * every rate frank-micro uses.  Run the tool instead.
 */
#ifndef TLV320DAC3100_H
#define TLV320DAC3100_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring the codec up for the given sample rate and route it to both the
 * headphone jack and the speaker amplifier.  Safe to call more than once.
 *
 * Returns false if the codec did not answer on I2C, having left the rest of the
 * system alone: audio is not worth halting the machine for.
 *
 * Note the PLL is clocked from the I2S bit clock, so it cannot lock until
 * something is driving BCLK.  Calling this before I2S starts is fine and is the
 * normal case; call tlv320_bclk_started() once BCLK is running.
 */
bool tlv320_init(uint32_t sample_rate_hz);

/*
 * Re-power the PLL now that BCLK is live, and wait for it to lock.  Call this
 * after i2s_init() and after any change to the I2S clock.  No-op if the codec
 * never initialised.
 */
void tlv320_bclk_started(void);

/*
 * Set the analogue output level, 0 to 100, where 100 is the codec's 0 dB and
 * lower values attenuate in the analogue domain after the DAC.
 *
 * Prefer this to scaling the samples.  The digital path then runs at full scale
 * and keeps all sixteen bits, and the attenuation happens in the output
 * amplifiers where it costs no resolution.  Cheap enough to call every buffer;
 * it returns immediately when the level is unchanged.
 */
void tlv320_set_volume(int percent);

/*
 * Unmute or mute both output amplifiers.  Muting the amps rather than feeding
 * silence is what removes idle hiss, and it also suppresses the click when the
 * active audio backend is switched.  Cheap enough to call every frame; it
 * returns immediately when the state is unchanged.
 */
void tlv320_set_muted(bool muted);

/* True once the codec has answered and been configured. */
bool tlv320_present(void);

#ifdef __cplusplus
}
#endif

#endif /* TLV320DAC3100_H */
