/*
 * frank-micro — BBC Micro for RP2350
 *
 * Copyright (c) 2026 Andrew de Quincey <adq@lidskialf.net>
 * https://github.com/rh1tech/frank-micro
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * board_fj.h — Adafruit Fruit Jam (product 6200, RP2350B) GPIO layout.
 *
 * DVI on GPIO 12..19, the same eight pins in the same order as the Murmulator
 * 2.0, so the PIO video drivers need no change.  SD card on hardware SPI0.
 * 8 MB PSRAM on GPIO 47.  16 MB flash.
 *
 * This board has no PS/2 socket and no NES/SNES pad connector, so a USB
 * keyboard is the only input path.  Its two USB-A sockets sit behind a CH334F
 * hub whose upstream pair is on GPIO 1 and 2, which are PIO pins and not the
 * RP2350's native USB controller.  The USB-C connector is wired as a device
 * (5.1K on both CC pins to ground) and cannot be made a host, so USB_HID=1 is
 * the only build that has a keyboard.
 *
 * Audio is embedded in the HDMI stream by default and needs nothing on the
 * board.  The headphone jack and the onboard speaker are both downstream of a
 * TLV320DAC3100 codec, which is an I2C slave at 0x18 and is silent until
 * configured; that driver does not exist yet.  There is no PWM audio output.
 *
 * There is no composite TV DAC and no VGA ribbon.
 *
 * Pin numbers were taken from the Pico SDK board header
 * boards/adafruit_fruit_jam.h and cross-checked against Adafruit's published
 * schematic.  See docs/FRUIT-JAM.md and docs/HANDOFF.md.
 *
 * Selected when -DPLATFORM=fj.
 */
#ifndef BOARD_FJ_H
#define BOARD_FJ_H

/* ---- Video capabilities ---- */
/* DVI on GPIO 12..19, each pin through one 220R resistor and nothing else.
 * No composite TV DAC, no VGA ribbon, so no HAS_TV and no runtime VGA
 * detection. */

/* ---- Audio capabilities ---- */
#define HAS_HDMI_AUDIO 1          /* data-island audio over the DVI connector */
#define HAS_I2S 1                 /* to the TLV320DAC3100; codec driver is phase 2 */
/* No HAS_PWM: the onboard speaker and headphone jack are both behind the
 * codec, so there is nowhere for a PWM backend to come out. */

/* ---- Input capabilities ---- */
/* Deliberately no HAS_PS2 and no HAS_NESPAD.  Neither connector exists, and
 * the absence of these macros is what gates the PS/2 and gamepad code out of
 * the build.  Adding either here would need pin numbers that do not exist. */

/* ---- HDMI / VGA pins ---- */
#define HDMI_BASE_PIN 12
#define VGA_BASE_PIN  12
/* Kept for any TU that still references the individual pair symbols.  The
 * odd-numbered pin of each pair is the positive half, which matches
 * board_m2.h exactly. */
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19

/* ---- SD Card (hardware SPI0) ----
 * These four values are also set from CMakeLists.txt as compile definitions,
 * and that copy is the one drivers/sdcard/sdcard.c actually sees.  Both must
 * agree. */
#define SDCARD_PIN_SPI0_SCK  34
#define SDCARD_PIN_SPI0_MOSI 35
#define SDCARD_PIN_SPI0_MISO 36
#define SDCARD_PIN_SPI0_CS   39
/* Card detect is a bare contact from the socket to the pin with no external
 * resistor, so it needs an internal pull-up.  Low means a card is present. */
#define SD_DETECT_PIN        33

/* ---- I2C0: audio codec control, also the STEMMA QT connector and the 2x16
 * header, so anything a user plugs in shares this bus ---- */
#define I2C_SDA_PIN      20
#define I2C_SCL_PIN      21
#define CODEC_I2C_ADDR   0x18

/* Shared reset for the codec and the ESP32-C6.  It has a 10K pull-up to 3V3,
 * so both parts leave reset at power-on with no firmware action: the hazard is
 * asserting this line, not forgetting to release it. */
#define PERIPH_RESET_PIN 22

/* ---- I2S audio, to the codec ----
 * BCLK 26 and WS 27 are adjacent, which is what drivers/audio.c requires.
 * MCLK on 25 is deliberately not driven: the codec's PLL runs from the bit
 * clock. */
#define I2S_DATA_PIN       24
#define I2S_CLOCK_PIN_BASE 26
#define I2S_MCLK_PIN       25

/* ---- PWM audio: no output on this board ----
 * drivers/pwm_audio/pwm_audio.c and frank_audio.c need these symbols to
 * compile.  The PWM backend is left out of the F12 menu on this platform and
 * is never initialised, so these pins are never driven.  They name two free
 * pins on the 2x16 header rather than anything that matters. */
#define PWM_PIN0 6
#define PWM_PIN1 7

/* ---- USB host: two USB-A sockets behind a CH334F hub on PIO pins ----
 * The hub sits on the switched VBUS rail, so it is unpowered until GPIO 11
 * goes high, and its own 12 MHz crystal then has to start.  It needs nothing
 * else: no reset pin and no configuration. */
#define USB_HOST_DP_PIN    1
#define USB_HOST_DM_PIN    2
#define USB_HOST_5V_EN_PIN 11

/* ---- Buttons: bare switches to ground, all three need internal pull-ups ----
 * BUTTON1 is also BOOTSEL, so holding it while plugging in USB always recovers
 * the board.  Which physical button is GPIO 4 and which is GPIO 5 is not
 * established: the schematic's net and part names are crossed. */
#define BUTTON1_PIN 0
#define BUTTON2_PIN 4
#define BUTTON3_PIN 5

/* ---- PSRAM (8 MB, always fitted, on GPIO 47) ----
 * GPIO 47 has no other use on this board, unlike the M1 where the PSRAM chip
 * select doubles as HDMI D0-.  BUTTER_PSRAM_GPIO is deliberately not defined:
 * its only reader is the ribbon-detection skip in drivers/test_pins.c, and
 * detection does not run here. */
#define PSRAM_CS_PIN_RP2350A 47
#define PSRAM_CS_PIN_RP2350B 47

#endif /* BOARD_FJ_H */
