/*
 * frank-micro — BBC Micro for RP2350 (b-em port)
 * frank_settings.h — mutable runtime settings.
 *
 * Persisted to /micro/micro.ini on the SD card.
 *
 * Model changes require a full BBC restart (model_init + main_reset).
 * Monitor, Sound, Volume and Limit Speed take effect immediately.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FRANK_SETTINGS_H
#define FRANK_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FRANK_SETTING_MODEL = 0,   /* BBC B / Master 128        (needs reboot)  */
    FRANK_SETTING_MONITOR,     /* Color / Green / Amber     (live)         */
    FRANK_SETTING_SOUND,       /* On / Off                  (live)         */
    FRANK_SETTING_VOLUME,      /* 0..100%                   (live)         */
    FRANK_SETTING_AUDIO,       /* HDMI / I2S / PWM output   (live)         */
    FRANK_SETTING_LIMIT_SPEED, /* On / Off                  (live)         */
    FRANK_SETTING_START_MODE,  /* Startup MODE 0..7 (links) (needs reset)  */
    FRANK_SETTING_CAPS_CTRL,   /* Normal / map CAPS+CTRL→A+S (live)        */
    FRANK_SETTING_DAC,         /* Printer-port DAC sound    (live)         */
    FRANK_SETTING_GAMEPAD,     /* NES/USB gamepad key mapping (live)        */
    FRANK_SETTING_COUNT,
} frank_setting_id_t;

/* Model indices — must match the model_NN sections in al_stub.cpp. */
typedef enum {
    FRANK_MODEL_B = 0,         /* BBC B with Acorn 1770 DFS */
    FRANK_MODEL_MASTER,        /* BBC Master 128            */
    FRANK_MODEL_COUNT,
} frank_model_t;

/* Audio-output backend. All three are switchable live from F12 (no restart).
 *   HDMI: audio embedded in the HDMI data-island stream (no extra wiring).
 *   I2S : external DAC on GPIO 9/10/11 (best quality).
 *   PWM : two-pin PWM into an RC low-pass filter on GPIO 10/11 (lo-fi).
 * I2S and PWM share GPIO 10/11, so only one drives the pins at a time. */
typedef enum {
    FRANK_AUDIO_HDMI = 0,
    FRANK_AUDIO_I2S,
    FRANK_AUDIO_PWM,
    FRANK_AUDIO_DRV_COUNT,
} frank_audio_driver_t;

/* Default audio backend depends on the selected video/audio driver:
 *   HDMI_PIO_AUDIO — audio is embedded in the HDMI stream (FRANK_AUDIO_HDMI).
 *   HDMI_PIO       — no HDMI-audio path exists, so default to the I2S DAC. */
#if defined(PLATFORM_PC)
/* Olimex PICO-PC: no I2S DAC; the analog jack (PWM) is the default output. */
#  define FRANK_AUDIO_DEFAULT FRANK_AUDIO_PWM
#elif defined(HDMI_PIO_AUDIO)
#  define FRANK_AUDIO_DEFAULT FRANK_AUDIO_HDMI
#else
#  define FRANK_AUDIO_DEFAULT FRANK_AUDIO_I2S
#endif

/* NES/SNES + USB gamepad → BBC key mapping presets. */
typedef enum {
    FRANK_GAMEPAD_OFF = 0,     /* gamepad ignored                          */
    FRANK_GAMEPAD_ARROWS,      /* D-pad → cursor keys, fire → RETURN/SPACE  */
    FRANK_GAMEPAD_ZXCOLON,     /* Z X : /  (common BBC game keys)           */
    FRANK_GAMEPAD_COUNT,
} frank_gamepad_map_t;

/* Logical gamepad actions resolved to a BBC key by the active preset. */
typedef enum {
    FRANK_PAD_UP = 0,
    FRANK_PAD_DOWN,
    FRANK_PAD_LEFT,
    FRANK_PAD_RIGHT,
    FRANK_PAD_FIRE1,
    FRANK_PAD_FIRE2,
    FRANK_PAD_START,
    FRANK_PAD_SELECT,
    FRANK_PAD_ACTION_COUNT,
} frank_pad_action_t;

typedef struct {
    uint8_t model;        /* frank_model_t                 */
    uint8_t monitor;      /* 0=Color, 1=Green, 2=Amber     */
    uint8_t sound;        /* 0=Off, 1=On                   */
    uint8_t volume;       /* 0..10 (x10 = 0%..100%)        */
    uint8_t audio_driver; /* frank_audio_driver_t: HDMI/I2S/PWM output  */
    uint8_t limit_speed;  /* 0=Off, 1=On                   */
    uint8_t start_mode;   /* 0..7 power-on screen MODE (keyboard links) */
    uint8_t caps_ctrl;    /* 0=Normal, 1=map CAPS/CTRL to A/S           */
    uint8_t dac;          /* 0=Off, 1=On — printer-port DAC sound       */
    uint8_t gamepad;      /* USB gamepad mapping preset (frank_gamepad_map_t) */
    char    disk_a[128];  /* auto-mount path for drive A   */
    char    disk_b[128];  /* auto-mount path for drive B   */
} frank_settings_t;

extern frank_settings_t g_frank_settings;

/* True when a restart-requiring setting changed but the BBC hasn't been
 * restarted yet.  Cleared by frank_settings_do_restart(). */
extern bool g_frank_settings_dirty;

/* Live values read by other modules. */
extern volatile int  g_frank_volume;        /* 0..100, read by frank_audio  */
extern volatile bool g_frank_sound_on;      /* read by frank_audio          */
extern volatile bool g_frank_limit_speed;   /* read by frank_perf_tick      */
extern volatile int  g_frank_audio_driver;  /* frank_audio_driver_t, read by frank_audio */

/* Select the active audio backend (HDMI/I2S/PWM).  Implemented in
 * frank_audio.c — lazily brings up the chosen driver and routes samples to
 * it.  Safe to call live (no restart needed). */
void frank_audio_set_driver(int drv);

/* Number of choices for a setting. */
int  frank_settings_choices(frank_setting_id_t id);
/* Label for the setting itself. */
const char *frank_settings_label(frank_setting_id_t id);
/* Label for the current value. */
const char *frank_settings_value_label(frank_setting_id_t id);
/* Cycle the value by delta (+1/-1). Wraps. Applies live settings immediately. */
void frank_settings_step(frank_setting_id_t id, int delta);
/* Restart level a setting change requires: 0=live, 1=reset, 2=reboot. */
int  frank_settings_restart_kind(frank_setting_id_t id);
/* True when the setting requires at least a BBC reset to take effect. */
bool frank_settings_needs_restart(frank_setting_id_t id);

/* Apply only the live visual/audio/input settings. */
void frank_settings_apply_live(void);

/* Boot-model index (read by the al_stub config stub). */
int  frank_boot_model(void);

/* ALLEGRO_KEY_* code that the given logical gamepad action maps to under the
 * active gamepad preset, or 0 if the action is unmapped / gamepad disabled. */
int  frank_gamepad_key_for(frank_pad_action_t action);
/* Non-zero when the gamepad mapping is enabled (preset != Off). */
int  frank_gamepad_enabled(void);

/* Boot keyboard-links value / startup MODE (read by the al_stub config stub). */
int  frank_boot_kbdips(void);

/* Full BBC restart applying the model setting.  Called from the UI when the
 * user selects "Apply & Restart", and by Ctrl+Alt+Del. */
void frank_settings_do_restart(void);

/* Load / save /micro/micro.ini. */
bool frank_settings_load(void);
bool frank_settings_save(void);

#ifdef __cplusplus
}
#endif

#endif /* FRANK_SETTINGS_H */
