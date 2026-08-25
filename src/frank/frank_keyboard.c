/*
 * frank-micro — BBC Micro for RP2350 (b-em port)
 * frank_keyboard.c — PS/2 keyboard -> b-em key matrix.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The frank PS/2 driver (ps2kbd_get_key) yields IBM PC XT-style scancodes.
 * b-em's key_down()/key_up() expect ALLEGRO_KEY_* codes, which it maps to the
 * BBC key matrix via keylookup.  We translate XT scancode -> ALLEGRO_KEY here.
 */
#include <stdint.h>
#include "pico.h"
#include "hardware/clocks.h"

#include "allegro5/allegro.h"   /* ALLEGRO_KEY_* */
#include "keyboard.h"           /* key_down(), key_up() */
#include "ps2kbd_wrapper.h"

#include "board_config.h"       /* NESPAD_GPIO_* pin assignments */
#include "nespad.h"             /* wired NES/SNES gamepad reader   */

#include "frank_gui.h"
#include "frank_ui.h"
#include "frank_screenshot.h"   /* g_frank_screenshot_pending */
#include "frank_settings.h"     /* frank_gamepad_key_for() — gamepad → BBC key */

#ifdef USB_HID_ENABLED
#include "usbhid.h"             /* usbhid_get_key_action() — raw HID usage */
#include "usbhid_wrapper.h"     /* usbhid_wrapper_tick(), _get_joystick()  */
#endif

extern void main_reset(void);   /* b-em soft reset (reboot the BBC) */
extern bool keyas;              /* map host A/S to BBC CAPS LOCK / CTRL */

/* XT scancode (0x00..0x7f) -> ALLEGRO_KEY_*; 0 = unmapped. */
static const uint8_t xt_to_allegro[128] = {
    [0x01] = ALLEGRO_KEY_ESCAPE,
    [0x02] = ALLEGRO_KEY_1, [0x03] = ALLEGRO_KEY_2, [0x04] = ALLEGRO_KEY_3,
    [0x05] = ALLEGRO_KEY_4, [0x06] = ALLEGRO_KEY_5, [0x07] = ALLEGRO_KEY_6,
    [0x08] = ALLEGRO_KEY_7, [0x09] = ALLEGRO_KEY_8, [0x0a] = ALLEGRO_KEY_9,
    [0x0b] = ALLEGRO_KEY_0,
    [0x0c] = ALLEGRO_KEY_MINUS, [0x0d] = ALLEGRO_KEY_EQUALS,
    [0x0e] = ALLEGRO_KEY_BACKSPACE, [0x0f] = ALLEGRO_KEY_TAB,
    [0x10] = ALLEGRO_KEY_Q, [0x11] = ALLEGRO_KEY_W, [0x12] = ALLEGRO_KEY_E,
    [0x13] = ALLEGRO_KEY_R, [0x14] = ALLEGRO_KEY_T, [0x15] = ALLEGRO_KEY_Y,
    [0x16] = ALLEGRO_KEY_U, [0x17] = ALLEGRO_KEY_I, [0x18] = ALLEGRO_KEY_O,
    [0x19] = ALLEGRO_KEY_P,
    [0x1a] = ALLEGRO_KEY_OPENBRACE, [0x1b] = ALLEGRO_KEY_CLOSEBRACE,
    [0x1c] = ALLEGRO_KEY_ENTER, [0x1d] = ALLEGRO_KEY_LCTRL,
    [0x1e] = ALLEGRO_KEY_A, [0x1f] = ALLEGRO_KEY_S, [0x20] = ALLEGRO_KEY_D,
    [0x21] = ALLEGRO_KEY_F, [0x22] = ALLEGRO_KEY_G, [0x23] = ALLEGRO_KEY_H,
    [0x24] = ALLEGRO_KEY_J, [0x25] = ALLEGRO_KEY_K, [0x26] = ALLEGRO_KEY_L,
    [0x27] = ALLEGRO_KEY_SEMICOLON, [0x28] = ALLEGRO_KEY_QUOTE,
    [0x29] = ALLEGRO_KEY_TILDE, [0x2a] = ALLEGRO_KEY_LSHIFT,
    [0x2b] = ALLEGRO_KEY_BACKSLASH,
    [0x2c] = ALLEGRO_KEY_Z, [0x2d] = ALLEGRO_KEY_X, [0x2e] = ALLEGRO_KEY_C,
    [0x2f] = ALLEGRO_KEY_V, [0x30] = ALLEGRO_KEY_B, [0x31] = ALLEGRO_KEY_N,
    [0x32] = ALLEGRO_KEY_M,
    [0x33] = ALLEGRO_KEY_COMMA, [0x34] = ALLEGRO_KEY_FULLSTOP,
    [0x35] = ALLEGRO_KEY_SLASH, [0x36] = ALLEGRO_KEY_RSHIFT,
    [0x37] = ALLEGRO_KEY_PAD_ASTERISK, [0x38] = ALLEGRO_KEY_ALT,
    [0x39] = ALLEGRO_KEY_SPACE, [0x3a] = ALLEGRO_KEY_CAPSLOCK,
    [0x3b] = ALLEGRO_KEY_F1, [0x3c] = ALLEGRO_KEY_F2, [0x3d] = ALLEGRO_KEY_F3,
    [0x3e] = ALLEGRO_KEY_F4, [0x3f] = ALLEGRO_KEY_F5, [0x40] = ALLEGRO_KEY_F6,
    [0x41] = ALLEGRO_KEY_F7, [0x42] = ALLEGRO_KEY_F8, [0x43] = ALLEGRO_KEY_F9,
    [0x44] = ALLEGRO_KEY_F10,
    [0x45] = ALLEGRO_KEY_NUMLOCK, [0x46] = ALLEGRO_KEY_SCROLLLOCK,
    [0x47] = ALLEGRO_KEY_PAD_7, [0x48] = ALLEGRO_KEY_PAD_8,
    [0x49] = ALLEGRO_KEY_PAD_9, [0x4a] = ALLEGRO_KEY_PAD_MINUS,
    [0x4b] = ALLEGRO_KEY_PAD_4, [0x4c] = ALLEGRO_KEY_PAD_5,
    [0x4d] = ALLEGRO_KEY_PAD_6, [0x4e] = ALLEGRO_KEY_PAD_PLUS,
    [0x4f] = ALLEGRO_KEY_PAD_1, [0x50] = ALLEGRO_KEY_PAD_2,
    [0x51] = ALLEGRO_KEY_PAD_3, [0x52] = ALLEGRO_KEY_PAD_0,
    [0x53] = ALLEGRO_KEY_PAD_DELETE,
    [0x57] = ALLEGRO_KEY_F11, [0x58] = ALLEGRO_KEY_F12,
    [0x59] = ALLEGRO_KEY_PAUSE, [0x5a] = ALLEGRO_KEY_UP,
    [0x5e] = ALLEGRO_KEY_INSERT, [0x5f] = ALLEGRO_KEY_DELETE,
    [0x61] = ALLEGRO_KEY_HOME, [0x62] = ALLEGRO_KEY_END,
    [0x63] = ALLEGRO_KEY_PGUP, [0x64] = ALLEGRO_KEY_PGDN,
    [0x65] = ALLEGRO_KEY_ALTGR, [0x66] = ALLEGRO_KEY_RCTRL,
    [0x67] = ALLEGRO_KEY_PAD_SLASH, [0x68] = ALLEGRO_KEY_PAD_ENTER,
    [0x6a] = ALLEGRO_KEY_DOWN, [0x6b] = ALLEGRO_KEY_LEFT,
    [0x6c] = ALLEGRO_KEY_RIGHT,
};

/* XT scancodes for the keys the UI cares about. */
#define XT_ESC       0x01
#define XT_ENTER     0x1c
#define XT_BACKSPACE 0x0e
#define XT_LCTRL     0x1d
#define XT_LALT      0x38
#define XT_F11       0x57
#define XT_F12       0x58
/* PrintScreen → screenshot.  Internal sentinel scancode (xt_to_allegro has no
 * entry for 0x54, so it never reaches the BBC matrix).  Must match sc_PrtSc in
 * drivers/ps2/ps2kbd_wrapper.c. */
#define XT_PRTSC     0x54
#define XT_UP        0x5a
#define XT_DELETE    0x5f
#define XT_PGUP      0x63
#define XT_PGDN      0x64
#define XT_RALT      0x65
#define XT_RCTRL     0x66
#define XT_DOWN      0x6a
#define XT_LEFT      0x6b
#define XT_RIGHT     0x6c

/* Translate an XT scancode to a FRANK_KS_* keysym for the overlay, or 0. */
static unsigned int xt_to_ks(unsigned char sc) {
    switch (sc) {
        case XT_ESC:       return FRANK_KS_Escape;
        case XT_ENTER:     return FRANK_KS_Return;
        case XT_BACKSPACE: return FRANK_KS_BackSpace;
        case XT_UP:        return FRANK_KS_Up;
        case XT_DOWN:      return FRANK_KS_Down;
        case XT_LEFT:      return FRANK_KS_Left;
        case XT_RIGHT:     return FRANK_KS_Right;
        case XT_PGUP:      return FRANK_KS_Page_Up;
        case XT_PGDN:      return FRANK_KS_Page_Down;
        case XT_DELETE:    return FRANK_KS_Delete;
        default:           return 0;
    }
}

/* Process one XT-scancode key event through the full frank pipeline:
 * Ctrl+Alt+Del reboot, F11/F12 overlays, overlay key routing, the
 * CAPS/CTRL→A/S remap, then key_down()/key_up() into b-em.  Shared by the
 * PS/2 and (when enabled) USB-HID keyboard sources.  *ctrl_held / *alt_held
 * persist across calls so the Ctrl+Alt+Del chord works across sources. */
static void process_xt_event(int pressed, unsigned char sc,
                             bool *ctrl_held, bool *alt_held) {
    if (sc >= 128) return;

    /* Track Ctrl/Alt for the Ctrl+Alt+Del reboot shortcut. */
    if (sc == XT_LCTRL || sc == XT_RCTRL) *ctrl_held = (pressed != 0);
    if (sc == XT_LALT  || sc == XT_RALT)  *alt_held  = (pressed != 0);

    if (pressed) {
        /* Ctrl+Alt+Del → reboot the BBC (instant soft reset). */
        if (sc == XT_DELETE && *ctrl_held && *alt_held) {
            main_reset();
            return;
        }
        /* F11 → media browser, F12 → settings overlay. */
        if (sc == XT_F11) { frank_ui_open_disk_menu(); return; }
        if (sc == XT_F12) { frank_ui_toggle();         return; }
        /* PrintScreen → save a screenshot of the live BBC frame. */
        if (sc == XT_PRTSC) { g_frank_screenshot_pending = true; return; }
        /* While an overlay is open, route presses to it (consume). */
        if (frank_ui_wants_keys()) {
            unsigned int ks = xt_to_ks(sc);
            if (ks) frank_ui_handle_key(ks);
            return;
        }
    }
    /* Release events fall through to b-em even while the overlay is open,
     * so a key held before the overlay appeared can't get stuck down. */

    int code = xt_to_allegro[sc];
    if (!code) return;
    /* "CAPS/CTRL Keys = A/S": the BBC's CAPS LK and CTRL sit where a PC
     * has Tab/CapsLock; this option lets them be reached via A and S.
     * Applied to both press and release so the keys can't stick. */
    if (keyas) {
        if (code == ALLEGRO_KEY_A)      code = ALLEGRO_KEY_CAPSLOCK;
        else if (code == ALLEGRO_KEY_S) code = ALLEGRO_KEY_LCTRL;
    }
    if (pressed)
        key_down(code);
    else
        key_up(code);
}

#ifdef USB_HID_ENABLED
/* USB HID usage ID (Usage Page 0x07) -> XT scancode; 0 = unmapped.
 * hid_app.c also emits the modifier pseudo-usages 0xE0..0xE2. */
static unsigned char hid_to_xt(uint8_t hid) {
    /* Modifiers (shared L/R) emitted by the driver. */
    switch (hid) {
        case 0xE0: return XT_LCTRL;          /* Ctrl  */
        case 0xE1: return 0x2a;              /* Shift (XT LSHIFT) */
        case 0xE2: return XT_LALT;           /* Alt   */
    }
    /* Letters A..Z (HID 0x04..0x1D). */
    if (hid >= 0x04 && hid <= 0x1D) {
        static const unsigned char letters[26] = {
            0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, /* A B C D E F G H */
            0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, /* I J K L M N O P */
            0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, /* Q R S T U V W X */
            0x15, 0x2c,                                     /* Y Z */
        };
        return letters[hid - 0x04];
    }
    /* Number row 1..9 then 0 (HID 0x1E..0x27). */
    if (hid >= 0x1E && hid <= 0x26) {
        static const unsigned char nums[9] = {
            0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
        };
        return nums[hid - 0x1E];
    }
    if (hid == 0x27) return 0x0b;            /* 0 */
    /* Function keys F1..F10 (HID 0x3A..0x43), F11/F12 (0x44/0x45). */
    if (hid >= 0x3A && hid <= 0x43) return (unsigned char)(0x3b + (hid - 0x3A));
    if (hid == 0x44) return XT_F11;
    if (hid == 0x45) return XT_F12;
    if (hid == 0x46) return XT_PRTSC;        /* PrintScreen → screenshot */

    switch (hid) {
        case 0x28: return XT_ENTER;          /* Return    */
        case 0x29: return XT_ESC;            /* Escape    */
        case 0x2A: return XT_BACKSPACE;      /* Backspace */
        case 0x2B: return 0x0f;              /* Tab       */
        case 0x2C: return 0x39;              /* Space     */
        case 0x2D: return 0x0c;              /* -         */
        case 0x2E: return 0x0d;              /* =         */
        case 0x2F: return 0x1a;              /* [         */
        case 0x30: return 0x1b;              /* ]         */
        case 0x31: return 0x2b;              /* backslash */
        case 0x33: return 0x27;              /* ;         */
        case 0x34: return 0x28;              /* '         */
        case 0x35: return 0x29;              /* `         */
        case 0x36: return 0x33;              /* ,         */
        case 0x37: return 0x34;              /* .         */
        case 0x38: return 0x35;              /* /         */
        case 0x39: return 0x3a;              /* CapsLock  */
        /* Navigation + arrows (extended XT codes used by xt_to_allegro). */
        case 0x49: return 0x5e;              /* Insert */
        case 0x4A: return 0x61;              /* Home   */
        case 0x4B: return XT_PGUP;           /* PageUp */
        case 0x4C: return XT_DELETE;         /* Delete */
        case 0x4D: return 0x62;              /* End    */
        case 0x4E: return XT_PGDN;           /* PageDn */
        case 0x4F: return XT_RIGHT;
        case 0x50: return XT_LEFT;
        case 0x51: return XT_DOWN;
        case 0x52: return XT_UP;
        case 0x58: return XT_ENTER;          /* Keypad Enter */
        default:   return 0;
    }
}

/* Drain queued USB-HID keyboard events and feed them through the shared
 * XT handler so USB keys behave identically to PS/2 keys. */
static void frank_usb_keyboard_poll(bool *ctrl_held, bool *alt_held) {
    uint8_t hid;
    int down;
    while (usbhid_get_key_action(&hid, &down)) {
        unsigned char sc = hid_to_xt(hid);
        if (sc) process_xt_event(down, sc, ctrl_held, alt_held);
    }
}
#endif /* USB_HID_ENABLED */

/* ── Gamepad (wired NES/SNES + optional USB HID) → BBC keys ─────────────── */

static bool s_nespad_ok = false;

void frank_gamepad_init(void) {
#ifdef HAS_NESPAD
    uint32_t cpu_khz = clock_get_hz(clk_sys) / 1000;
    s_nespad_ok = nespad_begin(cpu_khz,
                               NESPAD_GPIO_CLK,
                               NESPAD_GPIO_DATA,
                               NESPAD_DATA_PIN_NONE,
                               NESPAD_GPIO_LATCH);
#endif
}

/* Collapse a wired NES/SNES controller word into the FRANK_PAD_* action set. */
static void nespad_actions(uint32_t s, int *now) {
    if (s & DPAD_UP)    now[FRANK_PAD_UP]     = 1;
    if (s & DPAD_DOWN)  now[FRANK_PAD_DOWN]   = 1;
    if (s & DPAD_LEFT)  now[FRANK_PAD_LEFT]   = 1;
    if (s & DPAD_RIGHT) now[FRANK_PAD_RIGHT]  = 1;
    if (s & (DPAD_A | DPAD_Y)) now[FRANK_PAD_FIRE1] = 1;  /* primary fire */
    if (s & (DPAD_B | DPAD_X)) now[FRANK_PAD_FIRE2] = 1;  /* secondary    */
    if (s & DPAD_START)  now[FRANK_PAD_START]  = 1;
    if (s & DPAD_SELECT) now[FRANK_PAD_SELECT] = 1;
}

/* Poll all gamepad sources (wired NES/SNES pads on both ports, plus the USB
 * HID gamepad when enabled), merge them, and map onto BBC keys per the active
 * gamepad preset.  Edge detection ensures each press/release produces exactly
 * one key_down()/key_up(). */
static void frank_gamepad_poll(void) {
    static int prev[FRANK_PAD_ACTION_COUNT] = { 0 };
    int now[FRANK_PAD_ACTION_COUNT] = { 0 };

    /* Wired NES/SNES gamepads (both controller words). */
    if (s_nespad_ok) {
        nespad_read();
        nespad_actions(nespad_state,  now);
        nespad_actions(nespad_state2, now);
    }

#ifdef USB_HID_ENABLED
    /* USB HID gamepad (BTN_* mask, merged across slots). */
    {
        enum {
            BTN_LEFT  = 0x0001, BTN_RIGHT = 0x0002, BTN_UP    = 0x0004,
            BTN_DOWN  = 0x0008, BTN_FIREA = 0x0010, BTN_FIREB = 0x0020,
            BTN_START = 0x0100, BTN_SELECT= 0x0200, BTN_FIREX = 0x0800,
            BTN_FIREY = 0x1000,
        };
        unsigned int m = usbhid_wrapper_get_joystick();
        if (m & BTN_UP)               now[FRANK_PAD_UP]     = 1;
        if (m & BTN_DOWN)             now[FRANK_PAD_DOWN]   = 1;
        if (m & BTN_LEFT)             now[FRANK_PAD_LEFT]   = 1;
        if (m & BTN_RIGHT)            now[FRANK_PAD_RIGHT]  = 1;
        if (m & (BTN_FIREA|BTN_FIREY))now[FRANK_PAD_FIRE1]  = 1;
        if (m & (BTN_FIREB|BTN_FIREX))now[FRANK_PAD_FIRE2]  = 1;
        if (m & BTN_START)            now[FRANK_PAD_START]  = 1;
        if (m & BTN_SELECT)           now[FRANK_PAD_SELECT] = 1;
    }
#endif

    for (int a = 0; a < FRANK_PAD_ACTION_COUNT; ++a) {
        if (now[a] == prev[a]) continue;
        int code = frank_gamepad_key_for((frank_pad_action_t)a);
        if (code) {
            if (now[a]) key_down(code);
            else        key_up(code);
        }
        prev[a] = now[a];
    }
}

void frank_keyboard_poll(void) {
    static bool ctrl_held = false;
    static bool alt_held  = false;
    int pressed;
    unsigned char sc;

#ifdef USB_HID_ENABLED
    usbhid_wrapper_tick();   /* drive the TinyUSB host stack once per frame */
#endif

#ifdef HAS_PS2
    ps2kbd_tick();   /* drain PIO FIFO -> event queue */
    while (ps2kbd_get_key(&pressed, &sc))
        process_xt_event(pressed, sc, &ctrl_held, &alt_held);
#endif

#ifdef USB_HID_ENABLED
    frank_usb_keyboard_poll(&ctrl_held, &alt_held);
#endif
    frank_gamepad_poll();   /* wired NES/SNES pads (+ USB gamepad if enabled) */
}
