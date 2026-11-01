/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * frank-cpc - PWM Audio Driver
 *
 * Architecture (ported from frank-msx / murmnes):
 *   - One PWM slice drives the speaker pin(s) AND paces the DMA. It is
 *     configured so that exactly one WRAP occurs per output sample
 *     period (wrap = sys_clk / sample_rate - 1), giving ~13 bits of
 *     amplitude resolution at sys_clk=252 MHz / 22.05 kHz.
 *   - Each PWM WRAP raises DREQ_PWM_WRAPn, which fires a single 32-bit
 *     DMA write into the slice's packed CC register (low16 = CC_A,
 *     high16 = CC_B). The new CC value is latched at the following
 *     wrap (RP2 PWM is double-buffered), so amplitude updates every
 *     sample period without tearing.
 *   - Double-buffered DMA ring: CH_A runs buf 0, chains to CH_B which
 *     runs buf 1, which chains back to CH_A. An IRQ refills whichever
 *     buffer just completed. Mirrors the I2S driver pattern.
 *
 * IRQ selection: PIO HDMI uses DMA_IRQ_1 for video, I2S audio uses
 * DMA_IRQ_0. HSTX HDMI uses DMA_IRQ_0, I2S audio uses DMA_IRQ_1. The
 * PWM path picks DMA_IRQ_2 unconditionally so it never collides with
 * either active video driver or the other audio backend.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pwm_audio.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"

#include <stdio.h>
#include <string.h>

/* PWM wrap is computed at init to place one PWM cycle per sample period,
 * giving an amplitude resolution of sys_clk / sample_rate counts. At
 * 252 MHz / 22.05 kHz that's 11428 ≈ 13 bits. CC values are clamped
 * against the runtime-derived wrap. */
static uint32_t pwm_wrap = 4095;
static uint32_t pwm_center = 2047;

/* Buffer sized to hold ~2 full 50fps frames (2 × 882 = 1764 samples).
 * Capped at 1720 to stay within SRAM budget (5.5KB headroom on M2).
 * 2 DMA buffers × 1720 = ~78ms — gives nearly 2 frames of jitter headroom. */
#define DMA_BUFFER_SAMPLES 1720
#define DMA_BUFFER_COUNT   2
#define PREROLL_BUFFERS    2

/* DMA_IRQ_2 stays out of both video (IRQ_0/IRQ_1) and I2S audio paths. */
#define PWM_AUDIO_DMA_IRQ  DMA_IRQ_2
#define PWM_AUDIO_IRQ_IDX  2

/* Dedicated DMA channels — picked so they don't clash with HDMI (0,1) or
 * I2S (10,11). */
#define PWM_DMA_CH_A 8
#define PWM_DMA_CH_B 9

/* Packed CC word per sample: low16 = CC_A, high16 = CC_B. Same slice drives
 * both, or a single channel + a mirrored/parked second pin. */
static uint32_t __attribute__((aligned(4))) dma_bufs[DMA_BUFFER_COUNT][DMA_BUFFER_SAMPLES];

static volatile uint32_t bufs_free_mask = 0;
static volatile int preroll_count = 0;
static volatile bool dma_running = false;

static bool initialized = false;
static uint audio_slice = 0;
static bool stereo_same_slice = false;

/* Second channel on another slice (Olimex PICO-PC: left GPIO28 = slice 6 A,
 * right GPIO27 = slice 5 B).  A second ping-pong DMA pair (C/D), paced by
 * that slice's wrap DREQ, mirrors the same buffers into its CC register, so
 * both pins carry the audio.  Both slices run in phase from the same clock. */
static uint mirror_slice = 0;
static int pwm_dma_ch_c = -1;
static int pwm_dma_ch_d = -1;
static uint32_t cached_sample_rate = 0;
static volatile uint32_t dma_xfer_count = 0;

static void __not_in_flash_func(pwm_audio_dma_irq_handler)(void) {
    uint32_t ints = dma_hw->ints2;
    uint32_t mask = (1u << PWM_DMA_CH_A) | (1u << PWM_DMA_CH_B);
    if (pwm_dma_ch_c >= 0)
        mask |= (1u << pwm_dma_ch_c) | (1u << pwm_dma_ch_d);
    ints &= mask;
    if (!ints) return;

    /* Mirror pair: only re-arm (buffers are freed by the A/B pair). */
    if (pwm_dma_ch_c >= 0 && (ints & (1u << pwm_dma_ch_c))) {
        dma_hw->ints2 = (1u << pwm_dma_ch_c);
        dma_channel_set_read_addr(pwm_dma_ch_c, dma_bufs[0], false);
        dma_channel_set_trans_count(pwm_dma_ch_c, dma_xfer_count, false);
    }
    if (pwm_dma_ch_d >= 0 && (ints & (1u << pwm_dma_ch_d))) {
        dma_hw->ints2 = (1u << pwm_dma_ch_d);
        dma_channel_set_read_addr(pwm_dma_ch_d, dma_bufs[1], false);
        dma_channel_set_trans_count(pwm_dma_ch_d, dma_xfer_count, false);
    }

    if (ints & (1u << PWM_DMA_CH_A)) {
        dma_hw->ints2 = (1u << PWM_DMA_CH_A);
        dma_channel_set_read_addr(PWM_DMA_CH_A, dma_bufs[0], false);
        dma_channel_set_trans_count(PWM_DMA_CH_A, dma_xfer_count, false);
        bufs_free_mask |= 1u;
    }
    if (ints & (1u << PWM_DMA_CH_B)) {
        dma_hw->ints2 = (1u << PWM_DMA_CH_B);
        dma_channel_set_read_addr(PWM_DMA_CH_B, dma_bufs[1], false);
        dma_channel_set_trans_count(PWM_DMA_CH_B, dma_xfer_count, false);
        bufs_free_mask |= 2u;
    }
}

void pwm_audio_init(uint pin_l, uint pin_r, uint32_t sample_rate) {
    if (initialized) return;

    cached_sample_rate = sample_rate;

    /* Compute wrap so the PWM wraps exactly once per sample.
     * wrap_period_cycles = sys_clk / sample_rate, so wrap register = that - 1.
     * clkdiv stays at 1.0 so each PWM count is one sys_clk tick — the finest
     * amplitude resolution available. At 252 MHz / 44.1 kHz → wrap ≈ 5714
     * (~12 bits). */
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t wrap_plus_one = sys_clk / sample_rate;
    if (wrap_plus_one < 2) wrap_plus_one = 2;
    if (wrap_plus_one > 65536) wrap_plus_one = 65536;
    pwm_wrap = wrap_plus_one - 1;
    pwm_center = pwm_wrap / 2;

    /* Configure audio GPIOs as PWM. */
    gpio_set_function(pin_l, GPIO_FUNC_PWM);
    audio_slice = pwm_gpio_to_slice_num(pin_l);

    uint slice_r = pwm_gpio_to_slice_num(pin_r);
    stereo_same_slice = (slice_r == audio_slice);
    if (stereo_same_slice) {
        gpio_set_function(pin_r, GPIO_FUNC_PWM);
    } else {
        /* Different slice — configured like the audio slice and fed by the
         * mirror DMA pair below; enabled together with the audio slice. */
        pwm_config cfg2 = pwm_get_default_config();
        pwm_config_set_clkdiv(&cfg2, 1.0f);
        pwm_config_set_wrap(&cfg2, (uint16_t)pwm_wrap);
        gpio_set_function(pin_r, GPIO_FUNC_PWM);
        pwm_init(slice_r, &cfg2, false);
        pwm_set_both_levels(slice_r, (uint16_t)pwm_center, (uint16_t)pwm_center);
        mirror_slice = slice_r;
    }

    /* Configure audio slice: wraps at sample_rate, drives the pin AND
     * paces DMA via DREQ_PWM_WRAPn. */
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.0f);
    pwm_config_set_wrap(&cfg, (uint16_t)pwm_wrap);
    pwm_init(audio_slice, &cfg, false);
    pwm_set_both_levels(audio_slice, (uint16_t)pwm_center, (uint16_t)pwm_center);

    /* Pre-fill DMA buffers with mid-level silence */
    uint32_t silence = ((uint32_t)pwm_center << 16) | pwm_center;
    for (int b = 0; b < DMA_BUFFER_COUNT; b++)
        for (int i = 0; i < DMA_BUFFER_SAMPLES; i++)
            dma_bufs[b][i] = silence;

    /* CPC runs at 50 fps. */
    {
        uint32_t n = sample_rate / 50;
        if (n < 1) n = 1;
        if (n > DMA_BUFFER_SAMPLES) n = DMA_BUFFER_SAMPLES;
        dma_xfer_count = n;
    }

    /* Claim DMA channels and configure ping-pong chain */
    dma_channel_abort(PWM_DMA_CH_A);
    dma_channel_abort(PWM_DMA_CH_B);
    while (dma_channel_is_busy(PWM_DMA_CH_A) || dma_channel_is_busy(PWM_DMA_CH_B))
        tight_loop_contents();
    dma_channel_claim(PWM_DMA_CH_A);
    dma_channel_claim(PWM_DMA_CH_B);

    /* DREQ from the audio slice — one DMA transfer per PWM wrap, i.e. per
     * sample period. */
    uint dreq = DREQ_PWM_WRAP0 + audio_slice;

    dma_channel_config cfg_a = dma_channel_get_default_config(PWM_DMA_CH_A);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_a, dreq);
    channel_config_set_chain_to(&cfg_a, PWM_DMA_CH_B);

    dma_channel_config cfg_b = dma_channel_get_default_config(PWM_DMA_CH_B);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_b, dreq);
    channel_config_set_chain_to(&cfg_b, PWM_DMA_CH_A);

    /* Destination: packed CC register for the audio slice. Writing 32 bits
     * to &pwm_hw->slice[s].cc updates CC_A (low) and CC_B (high) in one go. */
    volatile uint32_t *cc_addr = &pwm_hw->slice[audio_slice].cc;

    dma_channel_configure(PWM_DMA_CH_A, &cfg_a, (void *)cc_addr,
                          dma_bufs[0], dma_xfer_count, false);
    dma_channel_configure(PWM_DMA_CH_B, &cfg_b, (void *)cc_addr,
                          dma_bufs[1], dma_xfer_count, false);

    if (!stereo_same_slice) {
        pwm_dma_ch_c = dma_claim_unused_channel(true);
        pwm_dma_ch_d = dma_claim_unused_channel(true);
        uint dreq_m = DREQ_PWM_WRAP0 + mirror_slice;
        volatile uint32_t *cc_m = &pwm_hw->slice[mirror_slice].cc;

        dma_channel_config cfg_c = dma_channel_get_default_config(pwm_dma_ch_c);
        channel_config_set_read_increment(&cfg_c, true);
        channel_config_set_write_increment(&cfg_c, false);
        channel_config_set_transfer_data_size(&cfg_c, DMA_SIZE_32);
        channel_config_set_dreq(&cfg_c, dreq_m);
        channel_config_set_chain_to(&cfg_c, pwm_dma_ch_d);

        dma_channel_config cfg_d = dma_channel_get_default_config(pwm_dma_ch_d);
        channel_config_set_read_increment(&cfg_d, true);
        channel_config_set_write_increment(&cfg_d, false);
        channel_config_set_transfer_data_size(&cfg_d, DMA_SIZE_32);
        channel_config_set_dreq(&cfg_d, dreq_m);
        channel_config_set_chain_to(&cfg_d, pwm_dma_ch_c);

        dma_channel_configure(pwm_dma_ch_c, &cfg_c, (void *)cc_m,
                              dma_bufs[0], dma_xfer_count, false);
        dma_channel_configure(pwm_dma_ch_d, &cfg_d, (void *)cc_m,
                              dma_bufs[1], dma_xfer_count, false);
    }

    dma_hw->ints2 = (1u << PWM_DMA_CH_A) | (1u << PWM_DMA_CH_B);
    if (pwm_dma_ch_c >= 0)
        dma_hw->ints2 = (1u << pwm_dma_ch_c) | (1u << pwm_dma_ch_d);
    irq_set_exclusive_handler(PWM_AUDIO_DMA_IRQ, pwm_audio_dma_irq_handler);
    irq_set_priority(PWM_AUDIO_DMA_IRQ, 0x80);
    irq_set_enabled(PWM_AUDIO_DMA_IRQ, true);
    dma_irqn_set_channel_enabled(PWM_AUDIO_IRQ_IDX, PWM_DMA_CH_A, true);
    dma_irqn_set_channel_enabled(PWM_AUDIO_IRQ_IDX, PWM_DMA_CH_B, true);
    if (pwm_dma_ch_c >= 0) {
        dma_irqn_set_channel_enabled(PWM_AUDIO_IRQ_IDX, pwm_dma_ch_c, true);
        dma_irqn_set_channel_enabled(PWM_AUDIO_IRQ_IDX, pwm_dma_ch_d, true);
    }

    preroll_count = 0;
    bufs_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    dma_running = false;

    /* Kick the audio slice — DREQ pulses start flowing immediately. The
     * DMA chain stays idle until preroll_count reaches PREROLL_BUFFERS
     * and commit_buf() calls dma_channel_start(). */
    if (stereo_same_slice) {
        pwm_set_enabled(audio_slice, true);
    } else {
        pwm_set_counter(audio_slice, 0);
        pwm_set_counter(mirror_slice, 0);
        pwm_set_mask_enabled(pwm_hw->en | (1u << audio_slice) | (1u << mirror_slice));
    }

    initialized = true;

    printf("PWM audio: init slice=%u rate=%lu wrap=%lu (~%u-bit)\n",
        audio_slice, (unsigned long)sample_rate, (unsigned long)pwm_wrap,
        (unsigned)(32u - __builtin_clz(pwm_wrap)));
}

/* Claim an idle DMA buffer. Returns the buffer index, or 0xFF if no
 * buffer is free (caller should drop samples). */
static uint8_t claim_buf(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    uint32_t free = bufs_free_mask;
    if (!dma_running) {
        uint8_t idx = (uint8_t)preroll_count;
        if (idx < DMA_BUFFER_COUNT && (free & (1u << idx))) {
            bufs_free_mask &= ~(1u << idx);
            restore_interrupts(irq_state);
            return idx;
        }
    } else if (free) {
        uint8_t idx = (free & 1u) ? 0 : 1;
        bufs_free_mask &= ~(1u << idx);
        restore_interrupts(irq_state);
        return idx;
    }
    restore_interrupts(irq_state);
    return 0xFF;
}

static void commit_buf(uint8_t idx, uint32_t frame_count) {
    /* Pad the rest of the DMA buffer with mid-level silence so we never
     * click on buffer underrun. */
    if (frame_count < dma_xfer_count) {
        uint32_t silence = ((uint32_t)pwm_center << 16) | pwm_center;
        for (uint32_t i = frame_count; i < dma_xfer_count; i++)
            dma_bufs[idx][i] = silence;
    }

    __dmb();

    if (!dma_running) {
        preroll_count++;
        if (preroll_count >= PREROLL_BUFFERS) {
            if (pwm_dma_ch_c >= 0)
                dma_start_channel_mask((1u << PWM_DMA_CH_A) | (1u << pwm_dma_ch_c));
            else
                dma_channel_start(PWM_DMA_CH_A);
            dma_running = true;
        }
    }
}

void pwm_audio_push_samples(const int16_t *buf, int count) {
    const int32_t center = (int32_t)pwm_center;
    const int32_t max_lvl = (int32_t)pwm_wrap;
    const int32_t swing = (int32_t)(pwm_wrap >> 1);

    int pos = 0;

    while (pos < count) {
        int chunk = count - pos;
        if (chunk > (int)dma_xfer_count) chunk = (int)dma_xfer_count;

        uint8_t idx = claim_buf();
        if (idx == 0xFF) {
            pos += chunk;
            continue;
        }
        uint32_t *dst = dma_bufs[idx];

        for (int i = 0; i < chunk; i++) {
            int32_t s = buf[pos + i];
            int32_t scaled = (s * swing) >> 15;
            int32_t lvl = scaled + center;
            if (lvl < 0) lvl = 0;
            if (lvl > max_lvl) lvl = max_lvl;
            uint32_t u = (uint32_t)lvl;
            dst[i] = (u << 16) | u;
        }

        commit_buf(idx, (uint32_t)chunk);
        pos += chunk;
    }
}

void pwm_audio_set_frame_rate(int frame_rate) {
    if (frame_rate <= 0) return;
    if (cached_sample_rate == 0) return;
    uint32_t n = cached_sample_rate / (uint32_t)frame_rate;
    if (n < 1) n = 1;
    if (n > DMA_BUFFER_SAMPLES) n = DMA_BUFFER_SAMPLES;
    dma_xfer_count = n;
}

void pwm_audio_set_chunk_frames(uint32_t frames) {
    if (frames < 1) frames = 1;
    if (frames > DMA_BUFFER_SAMPLES) frames = DMA_BUFFER_SAMPLES;
    dma_xfer_count = frames;
}

void pwm_audio_push_samples_blocking(const int16_t *buf, int count) {
    const int32_t center = (int32_t)pwm_center;
    const int32_t max_lvl = (int32_t)pwm_wrap;
    const int32_t swing = (int32_t)(pwm_wrap >> 1);

    int pos = 0;

    while (pos < count) {
        int chunk = count - pos;
        if (chunk > (int)dma_xfer_count) chunk = (int)dma_xfer_count;

        /* Wait for a free DMA buffer instead of dropping. The PWM-paced DMA
         * ring always keeps chaining (padding silence on underrun), so a
         * buffer is guaranteed to free up — no deadlock. */
        uint8_t idx;
        while ((idx = claim_buf()) == 0xFF)
            tight_loop_contents();

        uint32_t *dst = dma_bufs[idx];
        for (int i = 0; i < chunk; i++) {
            int32_t s = buf[pos + i];
            int32_t scaled = (s * swing) >> 15;
            int32_t lvl = scaled + center;
            if (lvl < 0) lvl = 0;
            if (lvl > max_lvl) lvl = max_lvl;
            uint32_t u = (uint32_t)lvl;
            dst[i] = (u << 16) | u;
        }

        commit_buf(idx, (uint32_t)chunk);
        pos += chunk;
    }
}

void pwm_audio_fill_silence(int count) {
    uint32_t silence = ((uint32_t)pwm_center << 16) | pwm_center;
    int remaining = count;
    while (remaining > 0) {
        int chunk = remaining;
        if (chunk > (int)dma_xfer_count) chunk = (int)dma_xfer_count;

        uint8_t idx = claim_buf();
        if (idx == 0xFF) {
            remaining -= chunk;
            continue;
        }
        uint32_t *dst = dma_bufs[idx];
        for (int i = 0; i < chunk; i++) dst[i] = silence;

        commit_buf(idx, (uint32_t)chunk);
        remaining -= chunk;
    }
}
