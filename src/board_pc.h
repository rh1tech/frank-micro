/*
 * frank-micro — BBC Micro for RP2350
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * board_pc.h — Olimex RP2040-PICO-PC with a Raspberry Pi Pico 2 (PCp2).
 *
 * Pinout as in the other PCp2 builds (murmapple, murmc64, pico-speccy).
 * Supported video: PIO HDMI (with or without HDMI-embedded audio).
 * Supported audio: HDMI / PWM (analog stereo jack). No I2S DAC on the board.
 *
 * Selected when -DPLATFORM=pc.
 */
#ifndef BOARD_PC_H
#define BOARD_PC_H

/* ---- Video capabilities ---- */
#define HAS_HDMI_AUDIO 1

/* ---- Audio capabilities ---- */
#define HAS_PWM 1

/* ---- HDMI pins: CLK 12/13, data pairs 14..19 ----
 * The Olimex board has the R/G TMDS lanes swapped relative to M2:
 * blue = 14/15, green = 18/19, red = 16/17 (libdvi order D0, D1, D2 =
 * 14, 18, 16, as in PICO-BK's olimex_rp2040_cfg). HDMI.c handles the swap
 * via PICO_PC; frank-hdmi-sound gets FRANK_HDMI_PIN_* from CMakeLists.txt. */
#define HDMI_BASE_PIN 12
#define VGA_BASE_PIN  12
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19
#define PICO_PC 1

/* ---- SD Card (SPI0) ---- */
#define SDCARD_PIN_SPI0_CS   22
#define SDCARD_PIN_SPI0_SCK  6
#define SDCARD_PIN_SPI0_MOSI 7
#define SDCARD_PIN_SPI0_MISO 4

/* ---- PS/2 keyboard (no mouse port) ---- */
#define PS2_PIN_CLK    0
#define PS2_PIN_DATA   1

/* ---- NES/SNES pad on UEXT: CLK UEXT-10, LATCH UEXT-5, DATA UEXT-3 ---- */
#define NESPAD_GPIO_CLK   5
#define NESPAD_GPIO_LATCH 9
#define NESPAD_GPIO_DATA  20

/* ---- PWM audio: right GPIO27, left GPIO28 (different PWM slices) ---- */
#define PWM_PIN0 28
#define PWM_PIN1 27

/* ---- I2S: no DAC on the board; defined for the build only, the I2S
 *      backend is never selected on this platform (frank_audio.c) ---- */
#define I2S_DATA_PIN       27
#define I2S_CLOCK_PIN_BASE 28

/* ---- PSRAM (optional): QSPI CS1 on GPIO8 ---- */
#define PSRAM_CS_PIN_RP2350A 8
#define PSRAM_CS_PIN_RP2350B 47

#define NO_UART_LOGGING 1

#endif /* BOARD_PC_H */
