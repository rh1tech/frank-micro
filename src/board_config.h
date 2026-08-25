/*
 * frank-micro — BBC Micro for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * board_config.h — Board configuration dispatcher.
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "pico.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"

#if defined(PLATFORM_M1)
#  include "board_m1.h"
#elif defined(PLATFORM_Z0)
#  include "board_z0.h"
#elif defined(PLATFORM_M2)
#  include "board_m2.h"
#elif defined(PLATFORM_FJ)
#  include "board_fj.h"
#else
#  error "No platform defined — set -DPLATFORM=m1|m2|z0|fj in CMake"
#endif

#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 252
#endif

#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

#ifndef CPU_VOLTAGE
#  if CPU_CLOCK_MHZ >= 504
#    define CPU_VOLTAGE VREG_VOLTAGE_1_65
#  elif CPU_CLOCK_MHZ >= 300
#    define CPU_VOLTAGE VREG_VOLTAGE_1_60
#  else
#    define CPU_VOLTAGE VREG_VOLTAGE_1_50
#  endif
#endif

/* PSRAM is optional — frank-micro tries to work without it */
#ifndef PSRAM_CS_PIN_RP2350A
#  define PSRAM_CS_PIN_RP2350A 0
#endif
#ifndef PSRAM_CS_PIN_RP2350B
#  define PSRAM_CS_PIN_RP2350B 0
#endif

#define PSRAM_PIN_RP2350A PSRAM_CS_PIN_RP2350A
#define PSRAM_PIN_RP2350B PSRAM_CS_PIN_RP2350B

static inline uint get_psram_pin(void) {
#if PICO_RP2350
    uint32_t package_sel = *((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    if (package_sel & 1) return PSRAM_CS_PIN_RP2350A;
    return PSRAM_CS_PIN_RP2350B;
#else
    return 0;
#endif
}

/* A board with a PS/2 keyboard socket but no mouse socket aliases the mouse
 * pins onto the keyboard's, which is what makes the mouse state machine in
 * ps2_init() harmless there.  A board with no PS/2 at all (HAS_PS2 undefined)
 * declares no PS/2 pins, so there is nothing to alias and every consumer is
 * gated out instead. */
#ifdef HAS_PS2
#  ifdef PS2_MOUSE_CLK
#    define HAS_PS2_MOUSE 1
#  else
#    define PS2_MOUSE_CLK  PS2_PIN_CLK
#    define PS2_MOUSE_DATA PS2_PIN_DATA
#  endif
#endif

/* BBC Micro framebuffer: 320×256 8-bit indexed colour.
 * BBC native modes: 640×256 (2MHz) or 320×256 (1MHz).
 * We down-sample to 320 wide, keep 256 lines. */
#define MICRO_FB_WIDTH    320
#define MICRO_FB_HEIGHT   256
#define MICRO_SCREEN_LINES 256

/* Legacy aliases kept for board_config consumers */
#define CPC_FB_WIDTH    MICRO_FB_WIDTH
#define CPC_FB_HEIGHT   MICRO_FB_HEIGHT
#define CPC_SCREEN_LINES MICRO_SCREEN_LINES

#endif /* BOARD_CONFIG_H */
