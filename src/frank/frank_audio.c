/*
 * frank-micro — BBC Micro for RP2350 (b-em port)
 * frank_audio.c — bridges b-em's audio buffer pool (sn76489 output) to the
 *                 frank HDMI audio ring.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * b-em fills a mono int16 buffer (sound_poll_n) and hands it back via
 * give_audio_buffer().  We up-mix to stereo and push it into the HDMI audio
 * ring; the encoder consumes it on core1.  Never blocks the producer.
 */
#include <stdint.h>
#include <string.h>
#include "pico.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/util/buffer.h"
#include "x_gui.h"

#include "board_config.h"
#include "tlv320dac3100.h"
#include "frank_settings.h"
#include "audio.h"          /* I2S DAC driver (drivers/audio.c)            */
#include "pwm_audio.h"      /* PWM audio driver (drivers/pwm_audio)        */

#if defined(HDMI_PIO_AUDIO)
#include "frank_hdmi.h"
#endif

#include <stdbool.h>

/* Live volume (0..100) and mute, driven by the settings UI (frank_settings.c). */
extern volatile int  g_frank_volume;
extern volatile bool g_frank_sound_on;

bool x_gui_audio_init_failed;

/* Active audio backend (frank_audio_driver_t).  Read by give_audio_buffer(),
 * written by frank_audio_set_driver().  Defaults to FRANK_AUDIO_DEFAULT: HDMI
 * data-island audio in the HDMI_PIO_AUDIO build, or the I2S DAC in the
 * HDMI_PIO build (which has no HDMI-audio path). */
volatile int g_frank_audio_driver = FRANK_AUDIO_DEFAULT;

/* ── I2S / PWM backends ────────────────────────────────────────────────────
 * The BBC SN76489 emits mono samples at FREQ_SO = 31250 Hz.  The HDMI path
 * resamples to the CEA-standard 32000 Hz (see below), but I2S and PWM are
 * free-running local DACs that lock to our own sysclk, so we drive them at
 * the native 31250 Hz directly — no resampling, no drift.
 *
 * I2S lives on PIO1 + DMA ch 10/11 (DMA_IRQ_0); PWM on DMA ch 8/9
 * (DMA_IRQ_2).  HDMI uses PIO0 + DMA ch 0-5 (DMA_IRQ_1).  No collisions.
 * Both I2S and PWM are brought up lazily the first time they are selected. */
#define FRANK_AUDIO_RATE 31250          /* == FREQ_SO (drivers run native)   */
#define FRANK_I2S_CHUNK  882            /* frames per i2s_dma_write           */
#define FRANK_AUDIO_SAMPLES 882         /* mono frames b-em delivers per give */

static bool          s_i2s_inited = false;
static i2s_config_t  s_i2s_cfg;
static bool          s_pwm_inited = false;

/* On the M2 and Z0 the I2S clock pins are also PWM_PIN0/1, and on the M1 they
 * are a different pair that still overlaps, so the two backends fight over the
 * pads.  Reassert the pin function whenever we switch, so the chosen driver
 * actually reaches the pads even after the other one claimed them.
 *
 * The Fruit Jam is the exception: I2S is on GPIO 24/26/27 to the codec and
 * there is no PWM output on the board at all, so nothing collides.  The
 * re-mux is harmless there and is kept for one code path.
 *
 * The PIO block has to match the one drivers/audio.c actually gave the state
 * machine; getting it wrong leaves the pads driven by the wrong peripheral. */
#if defined(PLATFORM_FJ)
#  define I2S_GPIO_FUNC GPIO_FUNC_PIO2
#else
#  define I2S_GPIO_FUNC GPIO_FUNC_PIO1
#endif

static void i2s_claim_pins(void) {
    gpio_set_function(I2S_DATA_PIN,           I2S_GPIO_FUNC);
    gpio_set_function(I2S_CLOCK_PIN_BASE,     I2S_GPIO_FUNC);
    gpio_set_function(I2S_CLOCK_PIN_BASE + 1, I2S_GPIO_FUNC);
}

static void pwm_claim_pins(void) {
    gpio_set_function(PWM_PIN0, GPIO_FUNC_PWM);
    gpio_set_function(PWM_PIN1, GPIO_FUNC_PWM);
}

static void ensure_i2s(void) {
    if (!s_i2s_inited) {
        s_i2s_cfg = i2s_get_default_config();
        s_i2s_cfg.sample_freq     = FRANK_AUDIO_RATE;
        s_i2s_cfg.dma_trans_count = FRANK_I2S_CHUNK;
        s_i2s_cfg.volume          = 0;   /* volume applied in software below */
        i2s_init(&s_i2s_cfg);            /* also sets the pins to a PIO function */
        s_i2s_inited = true;
        /* The codec's PLL is clocked from BCLK, so it could not lock until
         * now.  No-op on a board with no codec. */
        tlv320_bclk_started();
    } else {
        i2s_claim_pins();
    }
}

static void ensure_pwm(void) {
    if (!s_pwm_inited) {
        pwm_audio_init(PWM_PIN0, PWM_PIN1, FRANK_AUDIO_RATE);
        /* One full give-frame (882 samples) per DMA buffer so every buffer is
         * filled completely — no silence padding, no partial-chunk clicks. */
        pwm_audio_set_chunk_frames(FRANK_AUDIO_SAMPLES);
        s_pwm_inited = true;
    } else {
        pwm_claim_pins();
    }
}

/* Flush silence into the backend we are leaving so its free-running DMA does
 * not loop the last buffer (which would otherwise drone on a switch-away). */
static void i2s_quiet(void) {
    if (!s_i2s_inited) return;
    static int16_t zero[FRANK_I2S_CHUNK * 2];
    memset(zero, 0, sizeof(zero));
    /* Two writes guarantee both ping-pong buffers hold silence (i2s_dma_write
     * blocks until a buffer frees, so this is deterministic). */
    i2s_dma_write(&s_i2s_cfg, zero);
    i2s_dma_write(&s_i2s_cfg, zero);
}

static void pwm_quiet(void) {
    if (!s_pwm_inited) return;
    /* pwm_audio_fill_silence() drops when no buffer is free, so retry briefly
     * to make sure both DMA buffers end up silent. */
    for (int i = 0; i < 8; ++i) {
        pwm_audio_fill_silence(FRANK_I2S_CHUNK * 2);
        sleep_us(500);
    }
}

void frank_audio_set_driver(int drv) {
    if (drv < 0 || drv >= FRANK_AUDIO_DRV_COUNT) drv = FRANK_AUDIO_DEFAULT;
#if !defined(HDMI_PIO_AUDIO)
    /* No HDMI-audio backend in the HDMI_PIO build — route it to the I2S DAC. */
    if (drv == FRANK_AUDIO_HDMI) drv = FRANK_AUDIO_I2S;
#endif
    int old = g_frank_audio_driver;

    /* Silence the backend we are leaving (only matters when it keeps driving
     * its pins after the switch). */
    if (old == FRANK_AUDIO_I2S && drv != FRANK_AUDIO_I2S) {
        i2s_quiet();
        /* Mute the codec's output amplifiers too.  i2s_quiet() only pushes
         * silence; it does not stop the backend, so without this the amps stay
         * live and hissing after the switch. */
        tlv320_set_muted(true);
    }
    if (old == FRANK_AUDIO_PWM && drv != FRANK_AUDIO_PWM) pwm_quiet();

    switch (drv) {
        case FRANK_AUDIO_I2S:
            ensure_i2s();
            i2s_claim_pins();
            /* Unmute the codec's amplifiers once, here, rather than from the
             * producer path.  This is the only I2C the I2S backend does in
             * normal running, apart from a volume change. */
            tlv320_set_muted(false);
            break;
        case FRANK_AUDIO_PWM: ensure_pwm(); pwm_claim_pins(); break;
        case FRANK_AUDIO_HDMI:
        default:              break;   /* HDMI is always live on core1       */
    }

    g_frank_audio_driver = drv;
}

/* A small mono sample buffer reused each fill (b-em uses one in flight). */
static int16_t s_mono[FRANK_AUDIO_SAMPLES];
static mem_buffer_t s_mem = {
    .size = sizeof(s_mono),
    .bytes = (uint8_t *)s_mono,
    .flags = 0,
};
static struct audio_format s_fmt;
static struct audio_buffer_format s_bfmt = { .format = &s_fmt, .sample_stride = 2 };
static struct audio_buffer s_buf = {
    .buffer = &s_mem,
    .format = &s_bfmt,
    .max_sample_count = FRANK_AUDIO_SAMPLES,
};
static struct audio_buffer_pool s_pool;

struct audio_buffer_pool *x_gui_audio_init(uint freq) {
    s_fmt.sample_freq = freq;
    s_fmt.format = AUDIO_BUFFER_FORMAT_PCM_S16;
    s_fmt.channel_count = 1;
    x_gui_audio_init_failed = false;
    return &s_pool;
}

struct audio_buffer *take_audio_buffer(struct audio_buffer_pool *ac, bool block) {
    s_buf.sample_count = 0;
    return &s_buf;
}

/*
 * Push b-em's mono audio to the selected backend.
 *
 *   I2S / PWM: driven at the BBC's native 31250 Hz, so the samples are copied
 *              through 1:1 (with software volume) — no resampling.
 *
 *   HDMI:      resampled from 31250 Hz to the HDMI ring's standard 32000 Hz
 *              and up-mixed mono -> stereo.  The HDMI audio data-island stream
 *              advertises a STANDARD CEA-861 rate (32000 Hz) so real HDMI
 *              sinks lock their audio clock to our Clock-Regeneration packet.
 *              A non-standard rate (31250 Hz) is mishandled by many sinks,
 *              which then drop a sample every few seconds.
 *
 *              The ratio 31250/32000 = 0.9765625 = 64000/65536 exactly, so the
 *              linear resampler below is exact fixed-point: one output sample
 *              advances the input phase by 64000 in 16.16.
 */
#define RESAMP_STEP   64000u   /* (31250/32000) << 16, exact */
#define RESAMP_ONE    0x10000u

void give_audio_buffer(struct audio_buffer_pool *ac, struct audio_buffer *buffer) {
    const int16_t *src = (const int16_t *)buffer->buffer->bytes;
    uint32_t n = buffer->sample_count;

    int vol = g_frank_sound_on ? g_frank_volume : 0;
    if (vol > 100) vol = 100;

    int drv = g_frank_audio_driver;

    if (drv == FRANK_AUDIO_I2S) {
        if (!s_i2s_inited) return;

        /* Where the volume is applied depends on what is downstream.
         *
         * With a codec, hand it the samples at full scale and let its analogue
         * output stage do the attenuation: dividing 16-bit samples by up to 100
         * throws away most of the resolution, and doing it in the analogue
         * domain after the DAC costs none. Without a codec the I2S line feeds a
         * bare DAC with no volume control of its own, so the software divide is
         * the only option. */
        bool codec = tlv320_present();
        if (codec) tlv320_set_volume(vol);
        int digital_vol = codec ? 100 : vol;

        /* Native 31250 Hz: build one fixed FRANK_I2S_CHUNK stereo block. */
        static int16_t stereo[FRANK_I2S_CHUNK * 2];
        uint32_t cnt = (n > FRANK_I2S_CHUNK) ? FRANK_I2S_CHUNK : n;
        for (uint32_t i = 0; i < cnt; i++) {
            int16_t s = (digital_vol == 100)
                        ? src[i]
                        : (int16_t)(((int32_t)src[i] * digital_vol) / 100);
            stereo[i * 2]     = s;
            stereo[i * 2 + 1] = s;
        }
        for (uint32_t i = cnt; i < FRANK_I2S_CHUNK; i++) {
            stereo[i * 2] = 0;
            stereo[i * 2 + 1] = 0;
        }

        i2s_dma_write(&s_i2s_cfg, stereo);
        return;
    }

    if (drv == FRANK_AUDIO_PWM) {
        if (!s_pwm_inited) return;
        static int16_t mono[FRANK_AUDIO_SAMPLES];
        uint32_t cnt = (n > FRANK_AUDIO_SAMPLES) ? FRANK_AUDIO_SAMPLES : n;
        for (uint32_t i = 0; i < cnt; i++)
            mono[i] = (int16_t)(((int32_t)src[i] * vol) / 100);
        /* Blocking, exactly one full DMA buffer per call: self-paced to the
         * PWM output clock (like I2S) with no drops and no silence padding. */
        pwm_audio_push_samples_blocking(mono, (int)cnt);
        return;
    }

    /* HDMI (default) ─ resample 31250 -> 32000, mono -> stereo. */
#if defined(HDMI_PIO_AUDIO)
    static uint32_t mu   = 0;   /* 16.16 phase between prev and cur input  */
    static int16_t  prev = 0;   /* previous input sample (persists)        */
    static int16_t  stereo[256];
    uint32_t sc = 0;            /* stereo frames buffered for flush         */

    for (uint32_t i = 0; i < n; i++) {
        int16_t cur = (int16_t)(((int32_t)src[i] * vol) / 100);
        while (mu < RESAMP_ONE) {
            int32_t out = prev + (((int32_t)(cur - prev) * (int32_t)mu) >> 16);
            stereo[sc * 2]     = (int16_t)out;
            stereo[sc * 2 + 1] = (int16_t)out;
            if (++sc == 128) { frank_hdmi_audio_write(stereo, 128); sc = 0; }
            mu += RESAMP_STEP;
        }
        mu -= RESAMP_ONE;
        prev = cur;
    }
    if (sc) frank_hdmi_audio_write(stereo, sc);
#else
    (void)src; (void)n;
#endif
}

/* ── xip_stream stub ──────────────────────────────────────────────────────
 * b-em's audio.c streams flash-resident PCM samples (disc/ddnoise) via the
 * sector_read xip_stream DMA helper.  The BBC's BASIC beep uses sn76489 which
 * does NOT need this, so we stub the streamer for milestone 1.
 */
#include "xip_stream.h"
struct xip_stream_dma *xip_stream_head;
void xip_stream_init(void) {}
void xip_stream_dma_start(struct xip_stream_dma *dma) { if (dma) dma->state = COMPLETE; }
void xip_stream_dma_cancel(struct xip_stream_dma *dma) { if (dma) dma->state = NONE; }
void xip_stream_dma_poll(void) {}
