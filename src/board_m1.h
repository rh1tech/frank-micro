/*
 * frank-micro — BBC Micro for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * board_m1.h — Murmulator 1.x GPIO layout for frank-micro.
 *
 * Different pinout from M2. PIO-HDMI/VGA at GPIO 6-13.
 * No HSTX (HDMI_BASE_PIN != 12). No PS/2 mouse. No HDMI audio.
 *
 * Selected when -DPLATFORM=m1.
 */
#ifndef BOARD_M1_H
#define BOARD_M1_H

/* ---- Video capabilities ---- */
/* No HSTX — PIO HDMI/VGA only (GPIO 6-13) */
#define HAS_TV   1                /* Software composite on HDMI DAC pins */

/* ---- Audio capabilities ---- */
#define HAS_I2S 1                 /* External DAC on GPIO 26/27 */
#define HAS_PWM 1

/* ---- Input capabilities ---- */
#define HAS_PS2    1              /* PS/2 keyboard socket; no mouse socket */
#define HAS_NESPAD 1              /* NES/SNES pad connector */

/* ---- HDMI / VGA pins ---- */
#define HDMI_BASE_PIN 6
#define VGA_BASE_PIN  6

/* ---- Composite TV ---- */
#define TV_BASE_PIN 6

/* ---- SD Card (SPI0) ---- */
#define SDCARD_PIN_SPI0_SCK  2
#define SDCARD_PIN_SPI0_MOSI 3
#define SDCARD_PIN_SPI0_MISO 4
#define SDCARD_PIN_SPI0_CS   5

/* ---- PS/2 keyboard ---- */
#define PS2_PIN_CLK    0
#define PS2_PIN_DATA   1
/* No PS/2 mouse on M1 (GPIO 0 is KBD) */

/* ---- NES/SNES pad ---- */
#define NESPAD_GPIO_CLK   14
#define NESPAD_GPIO_LATCH 15
#define NESPAD_GPIO_DATA  16

/* ---- I2S audio ---- */
#define I2S_DATA_PIN       26
#define I2S_CLOCK_PIN_BASE 27

/* ---- PWM audio ---- */
#define PWM_PIN0 26
#define PWM_PIN1 27

/* ---- PSRAM (RP2350A vs RP2350B autodetect) ---- */
#define PSRAM_CS_PIN_RP2350A 8
#define PSRAM_CS_PIN_RP2350B 47

/* ---- UART logging ---- */
#define NO_UART_LOGGING 1

#endif /* BOARD_M1_H */
