/*
 * frank-micro — BBC Micro for RP2350 (b-em port)
 * frank_platform.c — board bring-up + entry point.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Core 0: hardware init, then the b-em emulator (CPU + synchronous video).
 * Core 1: frank HDMI encoder (frank_hdmi_run_core1), scanning out SCREEN[].
 *
 * We replace b-em's Allegro entry shim: al_stub's main() is disabled via
 * -DFRANK_MAIN and we call the b-em main (_al_mangled_main) ourselves after
 * bringing up the frank hardware.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/gpio.h"

#include "board_config.h"
#include "crash_handler.h"
#include "fpu_enable.h"
#include "HDMI.h"
#include "psram_init.h"
#include "ff.h"
#include "ps2kbd_wrapper.h"

#if defined(HDMI_PIO_AUDIO)
#include "frank_hdmi.h"
#endif

#include "frank_gui.h"
#include "frank_disc.h"
#include "frank_console.h"
#include "frank_settings.h"
#include "frank_loader.h"
#include "frank_ui.h"
#include "ui_draw.h"          /* boot error screen drawing primitives */
#include "usbhid_wrapper.h"   /* usbhid_wrapper_init() — stubs to {} when off */

#ifndef FRANK_MICRO_VERSION
#define FRANK_MICRO_VERSION "dev"
#endif

#define FB_W 320
#define FB_H 256

/* Crash diagnostics mirror (populated by crash_handler_check_and_print;
 * readable via the debug probe even when USB-CDC serial misses the print). */
volatile uint32_t g_crash_fault = 0;
volatile uint32_t g_crash_pc    = 0;
volatile uint32_t g_crash_cfsr  = 0;
volatile uint32_t g_crash_count = 0;

/* b-em entry point (main.c: #define main _al_mangled_main). */
extern int _al_mangled_main(int argc, char **argv);

/*
 * Per-frame pacing + speed measurement.  al_wait_for_event() calls this
 * exactly once per emulated 50Hz frame (each m6502_exec runs 40000 cycles ==
 * 20ms of BBC time).  We sleep until the next 20ms wall-clock boundary so the
 * emulator runs at exactly real-time (the asm CPU is faster than real-time at
 * 252MHz, so without this it would run ~6% fast).  Every wall-clock second we
 * also print the achieved frame rate and HDMI encoder heartbeat.
 */
#define FRAME_PERIOD_US 20000u   /* 50 Hz */

void frank_perf_tick(void) {
    static uint32_t frames = 0;
    static uint64_t t_last = 0;
    static uint64_t next_frame = 0;
    static bool     inited = false;

    /* One-shot init on the first frame, i.e. after b-em's main_init() has run
     * (so the disc subsystem and config are ready). */
    if (!inited) {
        inited = true;
        frank_settings_apply_live();   /* monitor palette, sound, volume, speed */
        frank_disk_autoload();         /* mount discs from settings / known names */
        if (frank_disc_name(0)) {
            frank_disc_request_boot();  /* SHIFT-BREAK an autoloaded disc */
        }
        /* No demo-disc fallback: with no disc configured the BBC boots to
         * its built-in BASIC prompt instead of auto-loading a bundled game. */
    }

    /* Release the autoboot SHIFT key once the disc has begun booting. */
#ifndef FRANK_NO_AUTOBOOT
    extern void frank_disc_autoboot_tick(void);
    frank_disc_autoboot_tick();
#endif

    /* Poll the USB-CDC serial console for injected keystrokes.  Disabled when
     * USB HID is enabled — the native USB port is then a HID host, not CDC. */
#ifndef USB_HID_ENABLED
    frank_console_poll();
#endif

    /* Feed the watchdog so a genuine hang (not a fault) is detected/rebooted. */
    crash_handler_feed();

    /* ── 50 Hz pacer ─────────────────────────────────────────────────────────
     *
     * Sleep until the next 20 ms wall-clock boundary so the emulator runs at
     * exactly real-time.  The audio producer (b-em) and the HDMI consumer run
     * off independent clocks, but the producer outputs the BBC's 31250 Hz
     * resampled to the consumer's standard 32000 Hz (frank_audio.c), so the
     * average rates match and the primed ring absorbs the jitter — no rate
     * lock needed (matches the working frank-cpc).
     *
     * When "Limit Speed" is Off the pacer is skipped so the emulator runs as
     * fast as the RP2350 allows (useful for fast loaders). */
    uint64_t now = time_us_64();
    if (!g_frank_limit_speed) {
        next_frame = 0;   /* re-prime when throttling resumes */
    } else if (next_frame == 0) {
        next_frame = now + FRAME_PERIOD_US;
    } else {
        if ((int64_t)(next_frame - now) > 0)
            busy_wait_until(from_us_since_boot(next_frame));
        next_frame += FRAME_PERIOD_US;
        /* Resync if we've fallen far behind (e.g. a slow frame). */
        now = time_us_64();
        if ((int64_t)(now - next_frame) > (int64_t)FRAME_PERIOD_US)
            next_frame = now + FRAME_PERIOD_US;
    }

    /* ── speed measurement ───────────────────────────────────────────────── */
    frames++;
    if (t_last == 0) { t_last = time_us_64(); return; }
    uint64_t dt = time_us_64() - t_last;
    if (dt >= 1000000u) {
#ifdef FRANK_PERF_LOG
        /*
         * Diagnostic telemetry.  OFF by default: printf() to USB-CDC can
         * block Core 0 for up to PICO_STDIO_USB_STDOUT_TIMEOUT_US whenever the
         * host has the port open but is not draining it, which stalls the
         * audio producer and causes a recurring dropout.  Enable only when a
         * terminal is actively reading the port.
         */
        float fps = frames * 1000000.0f / (float)dt;
        float pct = fps * 100.0f / 50.0f;  /* BBC frame rate is 50 Hz */
#if defined(HDMI_PIO_AUDIO)
        extern volatile uint32_t frank_hdmi_heartbeat_frames;
        static uint32_t hdmi_last = 0;
        uint32_t hdmi_now = frank_hdmi_heartbeat_frames;
        uint32_t hdmi_fps = hdmi_now - hdmi_last;
        hdmi_last = hdmi_now;
        printf("PERF: emu=%.1f fps (%.0f%%)  hdmi=%lu  fill=%lu/%lu  uf=%lu\n",
               (double)fps, (double)pct, (unsigned long)hdmi_fps,
               (unsigned long)frank_hdmi_audio_fill(),
               (unsigned long)frank_hdmi_audio_capacity(),
               (unsigned long)frank_hdmi_audio_underflows);
#else
        printf("PERF: emu=%.1f fps (%.0f%% real-time)\n", (double)fps, (double)pct);
#endif
#endif /* FRANK_PERF_LOG */
        frames = 0;
        t_last = time_us_64();
    }
}

/* ── Flash timing for overclocked operation ─────────────────────────────── */
static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int flash_max_mhz) {
    const int clock_hz  = cpu_mhz * 1000000;
    const int max_flash = flash_max_mhz * 1000000;
    int divisor = (clock_hz + max_flash - (max_flash >> 4) - 1) / max_flash;
    if (divisor < 1) divisor = 1;
    if (divisor == 1 && clock_hz >= 166000000) divisor = 2;
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) rxdelay++;
    qmi_hw->m[0].timing = 0x60007000u
        | ((uint32_t)rxdelay << QMI_M0_TIMING_RXDELAY_LSB)
        | ((uint32_t)divisor << QMI_M0_TIMING_CLKDIV_LSB);
}

/* Standard BBC physical-colour palette (indices 0..7). */
static void install_palette(void) {
    static const uint32_t bbc[8] = {
        0x000000, 0xFF0000, 0x00FF00, 0xFFFF00,
        0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    };
    for (int i = 0; i < 256; i++)
        graphics_set_palette((uint8_t)i, bbc[i & 7]);
}

/* ── Boot error screen ──────────────────────────────────────────────────
 *
 * Shown (and halts) when a required resource is missing.  frank-micro only
 * requires the SD card (ROMs are embedded in flash, PSRAM is optional), so
 * unlike frank-cpc there is no PSRAM check here.  Animated red plasma
 * background with the failure reason on top.
 */

/* Draw a text line with a black padded backdrop over the plasma background. */
static void boot_draw_line(uint8_t *fb, int x, int y, const char *text, uint8_t color) {
    int w = (int)strlen(text) * UI_CHAR_W;
    ui_fill_rect(fb, FB_W, x - 4, y - 2, w + 8, UI_CHAR_H + 4, UI_COLOR_BLACK);
    ui_draw_string(fb, FB_W, x, y, text, color);
}

static void boot_error_screen(FRESULT sd_result) {
    /* Sine LUT for plasma — in flash (static const) to avoid using RAM. */
    static const int8_t plasma_sin[256] = {
           0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
          49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
          90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
         117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
         127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
         117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
          90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
          49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
           0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
         -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
         -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
        -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
        -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
        -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
         -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
         -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
    };

    /* 64-color red plasma palette — black → deep red → bright red — in flash. */
    static const uint32_t plasma_pal[64] = {
        0x080000, 0x1a0000, 0x280000, 0x350000, 0x400100, 0x4b0100, 0x560200, 0x5f0300,
        0x690300, 0x720400, 0x7b0500, 0x830600, 0x8b0700, 0x920700, 0x990800, 0xa00900,
        0xa70a00, 0xad0b00, 0xb20b00, 0xb80c00, 0xbd0d00, 0xc10e00, 0xc50e00, 0xc90f00,
        0xcd0f00, 0xd01000, 0xd21000, 0xd41100, 0xd61100, 0xd81100, 0xd91100, 0xd91100,
        0xda1200, 0xd91100, 0xd91100, 0xd81100, 0xd61100, 0xd41100, 0xd21000, 0xd01000,
        0xcd0f00, 0xc90f00, 0xc50e00, 0xc10e00, 0xbd0d00, 0xb80c00, 0xb20b00, 0xad0b00,
        0xa70a00, 0xa00900, 0x990800, 0x920700, 0x8b0700, 0x830600, 0x7b0500, 0x720400,
        0x690300, 0x5f0300, 0x560200, 0x4b0100, 0x400100, 0x350000, 0x280000, 0x1a0000,
    };

    ui_draw_install_palette();

    /* Load plasma palette into slots 16-79 (BBC not running, these are free). */
    for (int i = 0; i < 64; i++)
        graphics_set_palette((uint8_t)(16 + i), plasma_pal[i]);

    /* Draw plasma into the framebuffer background (one-time). */
    for (int py = 0; py < FB_H; py++) {
        for (int px = 0; px < FB_W; px++) {
            int v = (int)plasma_sin[(uint8_t)(px * 2)]
                  + (int)plasma_sin[(uint8_t)(py * 2)]
                  + (int)plasma_sin[(uint8_t)(px + py)]
                  + (int)plasma_sin[(uint8_t)(px - py)];
            SCREEN[0][py * FB_W + px] = (uint8_t)(16 + (uint8_t)((v + 508) >> 4));
        }
    }
    graphics_set_buffer(SCREEN[0]);

    /* Draw error text on top — uses UI_COLOR_* indices 240-247. */
    const int x = 18;
    int y = 24;
    boot_draw_line(SCREEN[0], x, y, "frank-micro failed to start", UI_COLOR_FG);
    y += 18;

    if (sd_result != FR_OK) {
        char buf[48];
        snprintf(buf, sizeof(buf), "SD card: could not be mounted (error %d)", (int)sd_result);
        boot_draw_line(SCREEN[0], x, y, buf, UI_COLOR_ERROR);
        y += 10;
    } else {
        boot_draw_line(SCREEN[0], x, y, "SD card: OK", UI_COLOR_OK);
        y += 10;
    }

    y += 8;
    boot_draw_line(SCREEN[0], x, y, "The emulator could not be started.", UI_COLOR_FG);
    y += 10;
    boot_draw_line(SCREEN[0], x, y, "Please insert an SD card, then reboot.", UI_COLOR_FG);

    y += 20;
    boot_draw_line(SCREEN[0], x, y, "github.com/rh1tech/frank-micro", UI_COLOR_DIM);
    y += 10;
    {
        char info[52];
        snprintf(info, sizeof(info), "frank-micro v%s | (c) 2026 Mikhail Matveev",
                 FRANK_MICRO_VERSION);
        boot_draw_line(SCREEN[0], x, y, info, UI_COLOR_DIM);
    }

    /* Animate: cycle palette phase each frame — zero extra RAM, ~30 fps. */
    uint8_t phase = 0;
    while (true) {
        for (int i = 0; i < 64; i++)
            graphics_set_palette((uint8_t)(16 + i), plasma_pal[(uint8_t)(i + phase) & 63]);
        phase++;
        sleep_ms(33);
    }
}

static void boot_halt_if_required(FRESULT sd_result) {
    if (sd_result == FR_OK) return;
    boot_error_screen(sd_result);
}

int main(void) {
#if defined(PLATFORM_PC) && defined(PICO_SMPS_MODE_PIN)
    /* The PICO-PC carries a Raspberry Pi Pico 2, whose 3.3 V SMPS drops
     * into power-save (PFM) mode at light load unless GPIO23 is driven
     * high. At 252 MHz / default core voltage the load is light enough for
     * PFM, and its load-dependent ripple on 3.3 V reaches the PWM audio
     * outputs (PWM level = duty x 3.3 V) as hiss that follows every SD card
     * access or other activity. Forcing PWM mode (as the Pico 2 datasheet
     * recommends for low-noise analog work) keeps the rail quiet. The
     * emulators that run quietly on this board clock at 378-504 MHz with a
     * raised core voltage, where the SMPS stays out of PFM anyway. */
    gpio_init(PICO_SMPS_MODE_PIN);
    gpio_set_dir(PICO_SMPS_MODE_PIN, GPIO_OUT);
    gpio_put(PICO_SMPS_MODE_PIN, 1);
#endif
    frank_enable_fpu();
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ, FLASH_MAX_FREQ_MHZ);
    sleep_ms(100);
#endif
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false))
        set_sys_clock_khz(252 * 1000, true);

    stdio_init_all();
#ifndef USB_HID_ENABLED
    /* Wait for USB-CDC serial to enumerate so early printf output is visible.
     * Skipped when USB HID is enabled: the native USB port is in host mode,
     * there is no CDC device to wait for, and we want input ASAP. */
    for (int i = 0; i < 6; ++i) sleep_ms(250);  /* USB CDC enumeration */
#endif

    crash_handler_check_and_print();   /* report a fault from the previous run */

    printf("\n========================================\n");
    printf("  frank-micro — BBC Micro for RP2350 (b-em)\n");
    printf("  version %s\n", FRANK_MICRO_VERSION);
    printf("  cpu=%lu MHz\n", clock_get_hz(clk_sys) / 1000000u);
    printf("========================================\n");

    crash_handler_install();

#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif

    bool psram_ok = psram_init(get_psram_pin());
    printf("PSRAM: %s\n", psram_ok ? "OK" : "not present (OK)");

    ps2kbd_init();
    printf("PS/2 keyboard ready\n");

    /* USB HID host (keyboard + gamepad).  usbhid_wrapper_init() is a no-op
     * stub when USB HID is disabled, so this is safe to call unconditionally. */
    usbhid_wrapper_init();
#ifdef USB_HID_ENABLED
    printf("USB HID host ready\n");
#endif

    /* Wired NES/SNES gamepad (independent of USB HID; always available). */
    frank_gamepad_init();
    printf("NES gamepad ready\n");

    /* Video output selection.
     *  HDMI_PIO_AUDIO: force HDMI — the audio rides in the HDMI data-island
     *                  stream, so VGA (which has no audio path here) is never
     *                  selected (matches the frank capture-card pipeline).
     *  HDMI_PIO:       auto-detect HDMI vs VGA from the ribbon via testPins(),
     *                  exactly like frank-cpc; audio is on the local I2S/PWM DAC.
     */
#if defined(HDMI_PIO_AUDIO)
    SELECT_VGA = false;
#elif defined(VIDEO_COMPOSITE)
    /* Composite TV owns the video path on Core 1 (graphics_init launches it).
     * No HDMI/VGA ribbon autodetect — testPins() is stubbed in the TV shim. */
#else
    {
        int link = testPins(HDMI_BASE_PIN, HDMI_BASE_PIN + 1);
#if defined(PLATFORM_Z0) || defined(PLATFORM_PC)
        SELECT_VGA = false;            /* Z0 and Olimex PICO-PC: HDMI only */
#else
        SELECT_VGA = (link == 0) || (link == 0x1F);
#endif
        printf("Video: link=0x%02X -> %s\n", (unsigned)link,
               SELECT_VGA ? "VGA" : "HDMI");
    }
#endif
    graphics_init(g_out_HDMI);
    graphics_set_buffer(SCREEN[0]);
    graphics_set_res(FB_W, FB_H);
    graphics_set_shift((320 - FB_W) / 2, 0);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    install_palette();
    frank_ui_init();      /* install the overlay palette (indices 240-247) */
    printf("Video initialised (%dx%d)\n", FB_W, FB_H);

#if defined(HDMI_PIO_AUDIO)
    multicore_launch_core1(frank_hdmi_run_core1);
    sleep_ms(50);
    printf("HDMI encoder started on core1\n");
#endif

    /* SD card is required: ROMs are embedded in flash, but disc images and
     * settings live on the SD card.  Video + the HDMI encoder are already up,
     * so a missing card shows the boot error screen and halts. */
    static FATFS g_fs;
    FRESULT fr = f_mount(&g_fs, "", 1);
    printf("SD card mount: %s\n", fr == FR_OK ? "OK" : "not mounted");
    boot_halt_if_required(fr);

    /* Load persisted settings before b-em starts so frank_boot_model() picks
     * the configured model.  Discs are mounted on the first emulated frame
     * (frank_perf_tick), once b-em's disc subsystem is initialised. */
    if (fr == FR_OK)
        frank_settings_load();

    printf("Starting b-em...\n");
    _al_mangled_main(0, NULL);

    while (true) tight_loop_contents();
    return 0;
}
