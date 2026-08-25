/*
 * frank-micro — BBC Micro for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * board_z0.h — Waveshare RP2350-PiZero GPIO layout for frank-micro.
 *
 * PIO-HDMI only (HDMI_BASE_PIN=32, GPIO 32-39). No TV/VGA.
 * SD Card on SPI1 (pins above GPIO 28 require SPI1).
 * Uses waveshare_rp2350_pizero board definition for Pico SDK.
 *
 * Selected when -DPLATFORM=z0.
 */
#ifndef BOARD_Z0_H
#define BOARD_Z0_H

/* ---- Video capabilities ---- */
/* No HSTX. No TV/VGA. PIO-HDMI only (GPIO 32-39). */

/* ---- Audio capabilities ---- */
#define HAS_I2S 1                 /* I2S on GPIO 10/11 */
#define HAS_PWM 1

/* ---- Input capabilities ---- */
#define HAS_PS2    1              /* PS/2 keyboard socket; no mouse socket */
#define HAS_NESPAD 1              /* NES/SNES pad connector */

/* ---- HDMI / VGA pins ---- */
#define HDMI_BASE_PIN 32
#define VGA_BASE_PIN  32

/* ---- SD Card (hardware SPI1 — pins above 29 require SPI1) ---- */
#define SDCARD_SPI_BUS       spi1
#define SDCARD_PIN_SPI0_SCK  30
#define SDCARD_PIN_SPI0_MOSI 31
#define SDCARD_PIN_SPI0_MISO 40
#define SDCARD_PIN_SPI0_CS   43

/* ---- PS/2 keyboard ---- */
#define PS2_PIN_CLK   2
#define PS2_PIN_DATA  3
/* No PS/2 mouse on Z0 */

/* ---- NES/SNES pad ---- */
#define NESPAD_GPIO_CLK   4
#define NESPAD_GPIO_LATCH 5
#define NESPAD_GPIO_DATA  7

/* ---- I2S audio ---- */
#define I2S_DATA_PIN       10
#define I2S_CLOCK_PIN_BASE 11

/* ---- PWM audio ---- */
#define PWM_PIN0 10
#define PWM_PIN1 11

/* ---- PSRAM (built-in on GP47) ---- */
#define BUTTER_PSRAM_GPIO 47
#define PSRAM_CS_PIN_RP2350A 47
#define PSRAM_CS_PIN_RP2350B 47

/* ---- UART logging ---- */
#define NO_UART_LOGGING 1

#endif /* BOARD_Z0_H */
