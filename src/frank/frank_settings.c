/*
 * frank-micro — BBC Micro for RP2350 (b-em port)
 * frank_settings.c — runtime settings store, persistence and apply.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "frank_settings.h"

#include "ff.h"
#include "HDMI.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "allegro5/allegro.h"   /* ALLEGRO_KEY_* (gamepad → BBC key mapping) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── b-em globals we drive (extern-declared to avoid pulling Allegro) ──── */
extern bool sound_internal;         /* toggles the SN76489 beeper           */
extern bool sound_dac;              /* printer-port DAC sound source         */
extern bool keyas;                  /* map CAPS/CTRL to A/S                   */
extern int  kbdips;                 /* keyboard links (startup options)      */
extern void main_reset(void);       /* soft reset of the emulated BBC        */

/* ── live values read by other frank modules ──────────────────────────── */
volatile int  g_frank_volume      = 80;
volatile bool g_frank_sound_on    = true;
volatile bool g_frank_limit_speed = true;

/* ── defaults ──────────────────────────────────────────────────────────── */
frank_settings_t g_frank_settings = {
    .model       = FRANK_MODEL_MASTER,  /* boot as Master 128 by default */
    .monitor     = 0,                   /* Color */
    .sound       = 1,                   /* On    */
    .volume      = 8,                   /* 80%   */
    .audio_driver = FRANK_AUDIO_DEFAULT, /* HDMI data-island, or I2S in HDMI_PIO */
    .limit_speed = 1,                   /* On    */
    .start_mode  = 7,                   /* MODE 7 (standard BBC default) */
    .caps_ctrl   = 0,                   /* Normal */
    .dac         = 0,                   /* Off    */
    .gamepad     = FRANK_GAMEPAD_ZXCOLON, /* common BBC game keys (Z X : /) */
};

bool g_frank_settings_dirty = false;

/* Strongest pending restart level among changed-but-unapplied settings:
 * 0 = nothing, 1 = needs reset, 2 = needs reboot. */
static int s_pending_restart = 0;

/* ── value tables ──────────────────────────────────────────────────────── */
static const char *MODEL_LABELS[]   = { "BBC B", "Master 128" };
static const char *MONITOR_LABELS[] = { "Color", "Green", "Amber" };
static const char *ONOFF_LABELS[]   = { "Off", "On" };
static const char *CAPS_LABELS[]    = { "Normal", "A/S" };
static const char *GAMEPAD_LABELS[] = { "Off", "Arrows", "Z X : /" };
static const char *AUDIO_LABELS[]   = { "HDMI", "I2S", "PWM" };

/* Audio-driver choices exposed in the F12 menu depend on the build:
 *   HDMI_PIO_AUDIO — HDMI / I2S / PWM (HDMI data-island audio is available).
 *   HDMI_PIO, COMPOSITE — I2S / PWM only (no HDMI-embedded audio path), so
 *   the menu starts at FRANK_AUDIO_I2S and never offers "HDMI". */
#if defined(PLATFORM_FJ)
   /* Fruit Jam: the headphone jack and the onboard speaker are both behind the
    * TLV320DAC3100 codec, which is an I2S sink, so there is no PWM output on
    * the board at all and "PWM" comes out of the menu.  The enum order (HDMI,
    * I2S, PWM) means first=HDMI and count=2 gives exactly HDMI and I2S. */
#  if defined(HDMI_PIO_AUDIO)
#    define AUDIO_FIRST_CHOICE  FRANK_AUDIO_HDMI
#    define AUDIO_NUM_CHOICES   2
#  else
#    define AUDIO_FIRST_CHOICE  FRANK_AUDIO_I2S
#    define AUDIO_NUM_CHOICES   1
#  endif
#elif defined(HDMI_PIO_AUDIO)
#  define AUDIO_FIRST_CHOICE  FRANK_AUDIO_HDMI
#  define AUDIO_NUM_CHOICES   3
#else
#  define AUDIO_FIRST_CHOICE  FRANK_AUDIO_I2S
#  define AUDIO_NUM_CHOICES   2
#endif
static const char *MODE_LABELS[]    = {
    "MODE 0", "MODE 1", "MODE 2", "MODE 3",
    "MODE 4", "MODE 5", "MODE 6", "MODE 7"
};
static const char *VOLUME_LABELS[]  = {
    "0%", "10%", "20%", "30%", "40%", "50%",
    "60%", "70%", "80%", "90%", "100%"
};
#define VOLUME_STEPS 11

/* BBC physical-colour palette (must match install_palette() in frank_platform). */
static const uint32_t BBC_PALETTE[8] = {
    0x000000, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

/* ── API ───────────────────────────────────────────────────────────────── */

int frank_settings_choices(frank_setting_id_t id) {
    switch (id) {
        case FRANK_SETTING_MODEL:       return FRANK_MODEL_COUNT;
        case FRANK_SETTING_MONITOR:     return 3;
        case FRANK_SETTING_SOUND:       return 2;
        case FRANK_SETTING_VOLUME:      return VOLUME_STEPS;
        case FRANK_SETTING_AUDIO:       return AUDIO_NUM_CHOICES;
        case FRANK_SETTING_LIMIT_SPEED: return 2;
        case FRANK_SETTING_START_MODE:  return 8;
        case FRANK_SETTING_CAPS_CTRL:   return 2;
        case FRANK_SETTING_DAC:         return 2;
        case FRANK_SETTING_GAMEPAD:     return FRANK_GAMEPAD_COUNT;
        default:                        return 0;
    }
}

const char *frank_settings_label(frank_setting_id_t id) {
    switch (id) {
        case FRANK_SETTING_MODEL:       return "Model";
        case FRANK_SETTING_MONITOR:     return "Monitor";
        case FRANK_SETTING_SOUND:       return "Sound";
        case FRANK_SETTING_VOLUME:      return "Volume";
        case FRANK_SETTING_AUDIO:       return "Audio Out";
        case FRANK_SETTING_LIMIT_SPEED: return "Limit Speed";
        case FRANK_SETTING_START_MODE:  return "Startup Mode";
        case FRANK_SETTING_CAPS_CTRL:   return "CAPS/CTRL Keys";
        case FRANK_SETTING_DAC:         return "Printer DAC";
        case FRANK_SETTING_GAMEPAD:     return "Gamepad";
        default:                        return "?";
    }
}

const char *frank_settings_value_label(frank_setting_id_t id) {
    switch (id) {
        case FRANK_SETTING_MODEL:
            return MODEL_LABELS[g_frank_settings.model % FRANK_MODEL_COUNT];
        case FRANK_SETTING_MONITOR:
            return MONITOR_LABELS[g_frank_settings.monitor % 3];
        case FRANK_SETTING_SOUND:
            return ONOFF_LABELS[g_frank_settings.sound & 1];
        case FRANK_SETTING_VOLUME: {
            uint8_t idx = g_frank_settings.volume;
            if (idx >= VOLUME_STEPS) idx = VOLUME_STEPS - 1;
            return VOLUME_LABELS[idx];
        }
        case FRANK_SETTING_AUDIO: {
            int idx = (int)g_frank_settings.audio_driver;
            if (idx < AUDIO_FIRST_CHOICE ||
                idx >= AUDIO_FIRST_CHOICE + AUDIO_NUM_CHOICES)
                idx = AUDIO_FIRST_CHOICE;
            return AUDIO_LABELS[idx];
        }
        case FRANK_SETTING_LIMIT_SPEED:
            return ONOFF_LABELS[g_frank_settings.limit_speed & 1];
        case FRANK_SETTING_START_MODE:
            return MODE_LABELS[g_frank_settings.start_mode & 7];
        case FRANK_SETTING_CAPS_CTRL:
            return CAPS_LABELS[g_frank_settings.caps_ctrl & 1];
        case FRANK_SETTING_DAC:
            return ONOFF_LABELS[g_frank_settings.dac & 1];
        case FRANK_SETTING_GAMEPAD:
            return GAMEPAD_LABELS[g_frank_settings.gamepad % FRANK_GAMEPAD_COUNT];
        default:                        return "?";
    }
}

int frank_settings_restart_kind(frank_setting_id_t id) {
    switch (id) {
        case FRANK_SETTING_MODEL:      return 2;   /* full board reboot */
        case FRANK_SETTING_START_MODE: return 1;   /* BBC reset (re-read links) */
        default:                       return 0;   /* live */
    }
}

bool frank_settings_needs_restart(frank_setting_id_t id) {
    return frank_settings_restart_kind(id) > 0;
}

static void step_u8(uint8_t *v, int delta, int n) {
    int cur = (int)*v + delta;
    while (cur < 0)  cur += n;
    while (cur >= n) cur -= n;
    *v = (uint8_t)cur;
}

/* ── live apply helpers ────────────────────────────────────────────────── */

static uint32_t scale_rgb(uint32_t base, uint8_t monitor) {
    uint8_t r = (base >> 16) & 0xFF;
    uint8_t g = (base >> 8)  & 0xFF;
    uint8_t b =  base        & 0xFF;
    if (monitor == 0) return base;   /* Color */

    /* Luminance (Rec.601). */
    uint32_t lum = (uint32_t)((r * 77 + g * 150 + b * 29) >> 8);
    if (lum > 255) lum = 255;

    if (monitor == 1) {              /* Green monitor */
        return (uint32_t)lum << 8;
    }
    /* Amber monitor (~R full, G ~0.75, B 0). */
    uint32_t ga = (lum * 3) / 4;
    return ((uint32_t)lum << 16) | (ga << 8);
}

static void apply_monitor(void) {
    for (int i = 0; i < 8; ++i)
        graphics_set_palette((uint8_t)i, scale_rgb(BBC_PALETTE[i], g_frank_settings.monitor));
}

void frank_settings_apply_live(void) {
#if !defined(HDMI_PIO_AUDIO)
    /* HDMI audio is unavailable in the HDMI_PIO build; fold any persisted
     * "HDMI" selection (e.g. from a micro.ini written by an HDMI_PIO_AUDIO
     * build) onto the I2S DAC so the option is never silently dead. */
    if (g_frank_settings.audio_driver == FRANK_AUDIO_HDMI)
        g_frank_settings.audio_driver = FRANK_AUDIO_I2S;
#endif
    apply_monitor();
    g_frank_sound_on    = (g_frank_settings.sound != 0);
    sound_internal      = g_frank_sound_on;
    g_frank_volume      = (int)g_frank_settings.volume * 10;
    frank_audio_set_driver(g_frank_settings.audio_driver);
    g_frank_limit_speed = (g_frank_settings.limit_speed != 0);
    sound_dac           = (g_frank_settings.dac != 0);
    keyas               = (g_frank_settings.caps_ctrl != 0);
    kbdips              = g_frank_settings.start_mode & 7;
}

void frank_settings_step(frank_setting_id_t id, int delta) {
    int n = frank_settings_choices(id);
    if (n <= 0) return;
    int kind = frank_settings_restart_kind(id);
    if (kind > 0) {
        g_frank_settings_dirty = true;
        if (kind > s_pending_restart) s_pending_restart = kind;
    }

    switch (id) {
        case FRANK_SETTING_MODEL:
            step_u8(&g_frank_settings.model, delta, n);
            break;
        case FRANK_SETTING_MONITOR:
            step_u8(&g_frank_settings.monitor, delta, n);
            apply_monitor();
            break;
        case FRANK_SETTING_SOUND:
            step_u8(&g_frank_settings.sound, delta, n);
            g_frank_sound_on = (g_frank_settings.sound != 0);
            sound_internal   = g_frank_sound_on;
            break;
        case FRANK_SETTING_VOLUME:
            step_u8(&g_frank_settings.volume, delta, n);
            g_frank_volume = (int)g_frank_settings.volume * 10;
            break;
        case FRANK_SETTING_AUDIO: {
            /* Cycle within the build's allowed audio backends only — the
             * "HDMI" option is excluded entirely in non-HDMI-audio builds. */
            int cur = (int)g_frank_settings.audio_driver - AUDIO_FIRST_CHOICE;
            if (cur < 0 || cur >= AUDIO_NUM_CHOICES) cur = 0;
            cur += delta;
            while (cur < 0)                 cur += AUDIO_NUM_CHOICES;
            while (cur >= AUDIO_NUM_CHOICES) cur -= AUDIO_NUM_CHOICES;
            g_frank_settings.audio_driver = (uint8_t)(AUDIO_FIRST_CHOICE + cur);
            frank_audio_set_driver(g_frank_settings.audio_driver);
            break;
        }
        case FRANK_SETTING_LIMIT_SPEED:
            step_u8(&g_frank_settings.limit_speed, delta, n);
            g_frank_limit_speed = (g_frank_settings.limit_speed != 0);
            break;
        case FRANK_SETTING_START_MODE:
            step_u8(&g_frank_settings.start_mode, delta, n);
            kbdips = g_frank_settings.start_mode & 7;   /* takes effect on reset */
            break;
        case FRANK_SETTING_CAPS_CTRL:
            step_u8(&g_frank_settings.caps_ctrl, delta, n);
            keyas = (g_frank_settings.caps_ctrl != 0);
            break;
        case FRANK_SETTING_DAC:
            step_u8(&g_frank_settings.dac, delta, n);
            sound_dac = (g_frank_settings.dac != 0);
            break;
        case FRANK_SETTING_GAMEPAD:
            step_u8(&g_frank_settings.gamepad, delta, n);
            break;
        default: break;
    }
    frank_settings_save();
}

int frank_boot_model(void) {
    return g_frank_settings.model % FRANK_MODEL_COUNT;
}

/* ── USB gamepad → BBC key mapping ─────────────────────────────────────── */

int frank_gamepad_enabled(void) {
    return g_frank_settings.gamepad != FRANK_GAMEPAD_OFF;
}

int frank_gamepad_key_for(frank_pad_action_t action) {
    if (action < 0 || action >= FRANK_PAD_ACTION_COUNT) return 0;

    switch (g_frank_settings.gamepad) {
        case FRANK_GAMEPAD_ARROWS:
            switch (action) {
                case FRANK_PAD_UP:     return ALLEGRO_KEY_UP;
                case FRANK_PAD_DOWN:   return ALLEGRO_KEY_DOWN;
                case FRANK_PAD_LEFT:   return ALLEGRO_KEY_LEFT;
                case FRANK_PAD_RIGHT:  return ALLEGRO_KEY_RIGHT;
                case FRANK_PAD_FIRE1:  return ALLEGRO_KEY_ENTER;  /* RETURN */
                case FRANK_PAD_FIRE2:  return ALLEGRO_KEY_SPACE;
                case FRANK_PAD_START:  return ALLEGRO_KEY_ENTER;  /* RETURN */
                case FRANK_PAD_SELECT: return ALLEGRO_KEY_ESCAPE;
                default:               return 0;
            }
        case FRANK_GAMEPAD_ZXCOLON:
            /* Common BBC game keys: Z=left, X=right, :=up, /=down.
             * The BBC ":" key is host ALLEGRO_KEY_QUOTE (see keydef-allegro.c). */
            switch (action) {
                case FRANK_PAD_UP:     return ALLEGRO_KEY_QUOTE;  /* : */
                case FRANK_PAD_DOWN:   return ALLEGRO_KEY_SLASH;  /* / */
                case FRANK_PAD_LEFT:   return ALLEGRO_KEY_Z;
                case FRANK_PAD_RIGHT:  return ALLEGRO_KEY_X;
                case FRANK_PAD_FIRE1:  return ALLEGRO_KEY_ENTER;  /* RETURN */
                case FRANK_PAD_FIRE2:  return ALLEGRO_KEY_SPACE;
                case FRANK_PAD_START:  return ALLEGRO_KEY_ENTER;  /* RETURN */
                case FRANK_PAD_SELECT: return ALLEGRO_KEY_ESCAPE;
                default:               return 0;
            }
        case FRANK_GAMEPAD_OFF:
        default:
            return 0;
    }
}

int frank_boot_kbdips(void) {
    return g_frank_settings.start_mode & 7;
}

void frank_settings_do_restart(void) {
    /* Persist first so the (possibly new) model survives a reboot. */
    frank_settings_save();

    if (s_pending_restart >= 2) {
        /* A reboot-requiring setting (the model) changed.  b-em allocates
         * sideways-RAM banks at model_init() time and never frees them, so a
         * soft main_restart() would leak/double-allocate.  Reboot the whole
         * board instead: on the next boot frank_boot_model() selects the new
         * model and b-em initialises it from a clean slate. */
        g_frank_settings_dirty = false;
        s_pending_restart = 0;
        printf("settings: rebooting to apply model = %s\n",
               MODEL_LABELS[g_frank_settings.model % FRANK_MODEL_COUNT]);
        sleep_ms(60);
        watchdog_reboot(0, 0, 0);
        while (1) tight_loop_contents();
    }

    /* A plain reset of the emulated machine applies the remaining changes
     * (e.g. the startup MODE is re-read from the keyboard links at reset). */
    g_frank_settings_dirty = false;
    s_pending_restart = 0;
    printf("settings: resetting BBC\n");
    main_reset();
    frank_settings_apply_live();
}

/* ── INI persistence ───────────────────────────────────────────────────── */

#define FRANK_INI_PATH "/micro/micro.ini"

typedef struct {
    const char *key;
    uint8_t    *slot;
    uint8_t     max;
} ini_field_t;

static const ini_field_t INI_FIELDS[] = {
    { "model",       &g_frank_settings.model,       FRANK_MODEL_COUNT },
    { "monitor",     &g_frank_settings.monitor,     3 },
    { "sound",       &g_frank_settings.sound,       2 },
    { "volume",      &g_frank_settings.volume,      VOLUME_STEPS },
    { "audio_driver", &g_frank_settings.audio_driver, FRANK_AUDIO_DRV_COUNT },
    { "limit_speed", &g_frank_settings.limit_speed, 2 },
    { "start_mode",  &g_frank_settings.start_mode,  8 },
    { "caps_ctrl",   &g_frank_settings.caps_ctrl,   2 },
    { "dac",         &g_frank_settings.dac,         2 },
    { "gamepad",     &g_frank_settings.gamepad,     FRANK_GAMEPAD_COUNT },
};
#define INI_FIELD_COUNT ((int)(sizeof(INI_FIELDS)/sizeof(INI_FIELDS[0])))

static void ini_trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
}

bool frank_settings_load(void) {
    FIL f;
    if (f_open(&f, FRANK_INI_PATH, FA_READ) != FR_OK) {
        printf("settings: no micro.ini, using defaults\n");
        return false;
    }
    char line[160];
    while (f_gets(line, sizeof(line), &f)) {
        ini_trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        ini_trim(key); ini_trim(val);

        if (strcmp(key, "disk_a") == 0 || strcmp(key, "disk_b") == 0) {
            char *dst = (key[5] == 'a') ? g_frank_settings.disk_a : g_frank_settings.disk_b;
            size_t dsz = sizeof(g_frank_settings.disk_a);
            if (val[0] != '/') snprintf(dst, dsz, "/micro/disk/%s", val);
            else { strncpy(dst, val, dsz - 1); dst[dsz - 1] = 0; }
            continue;
        }

        for (int i = 0; i < INI_FIELD_COUNT; ++i) {
            if (strcmp(key, INI_FIELDS[i].key) == 0) {
                char *endp = NULL;
                long v = strtol(val, &endp, 10);
                if (endp && endp != val && v >= 0 && v < INI_FIELDS[i].max)
                    *INI_FIELDS[i].slot = (uint8_t)v;
                break;
            }
        }
    }
    f_close(&f);
    printf("settings: loaded %s\n", FRANK_INI_PATH);
    return true;
}

bool frank_settings_save(void) {
    FIL f;
    if (f_open(&f, FRANK_INI_PATH, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        printf("settings: save failed\n");
        return false;
    }
    /* Large enough for the longest line: "disk_a=" + 127-char path + "\n". */
    char buf[160];
    UINT bw;
    int n = snprintf(buf, sizeof(buf), "# frank-micro settings\n");
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    f_write(&f, buf, (UINT)n, &bw);
    for (int i = 0; i < INI_FIELD_COUNT; ++i) {
        n = snprintf(buf, sizeof(buf), "%s=%u\n",
                     INI_FIELDS[i].key, (unsigned)*INI_FIELDS[i].slot);
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        f_write(&f, buf, (UINT)n, &bw);
    }
    if (g_frank_settings.disk_a[0]) {
        n = snprintf(buf, sizeof(buf), "disk_a=%s\n", g_frank_settings.disk_a);
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        f_write(&f, buf, (UINT)n, &bw);
    }
    if (g_frank_settings.disk_b[0]) {
        n = snprintf(buf, sizeof(buf), "disk_b=%s\n", g_frank_settings.disk_b);
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        f_write(&f, buf, (UINT)n, &bw);
    }
    f_close(&f);
    return true;
}
