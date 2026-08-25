# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
#
# frank-micro — BBC Micro for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-micro
# SPDX-License-Identifier: GPL-3.0-or-later
#

cmake_minimum_required(VERSION 3.13)

# Platform selection: m1, m2, z0, fj
set(PLATFORM "m2" CACHE STRING "Target platform: m1, m2, z0, fj")
set_property(CACHE PLATFORM PROPERTY STRINGS m1 m2 z0 fj)

if(NOT PLATFORM STREQUAL "m1" AND NOT PLATFORM STREQUAL "m2"
   AND NOT PLATFORM STREQUAL "z0" AND NOT PLATFORM STREQUAL "fj")
    message(FATAL_ERROR "Unknown PLATFORM '${PLATFORM}' — must be m1, m2, z0, or fj")
endif()

set(CPU_SPEED "252" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "100" CACHE STRING "PSRAM max frequency in MHz (unused, no PSRAM required)")
set(FLASH_SPEED "66"  CACHE STRING "Flash target max frequency in MHz")

# Video driver: HDMI_PIO_AUDIO (HDMI-embedded audio), HDMI_PIO (PIO HDMI/VGA +
# I2S/PWM audio) or COMPOSITE (software PAL/NTSC TV + I2S/PWM audio).  Declared
# here — before the voltage is derived below — so COMPOSITE can raise CPU_SPEED.
set(HDMI_DRIVER "HDMI_PIO_AUDIO" CACHE STRING "HDMI driver: HDMI_PIO_AUDIO, HDMI_PIO or COMPOSITE")
set_property(CACHE HDMI_DRIVER PROPERTY STRINGS "HDMI_PIO_AUDIO" "HDMI_PIO" "COMPOSITE")

# Composite TV is software-timed on Core 1 and not wired on the Z0 board.
if(HDMI_DRIVER STREQUAL "COMPOSITE" AND PLATFORM STREQUAL "z0")
    message(FATAL_ERROR "Composite TV is not supported on the Z0 platform")
endif()

# The Fruit Jam has no video DAC on the DVI pins and no composite connector.
if(HDMI_DRIVER STREQUAL "COMPOSITE" AND PLATFORM STREQUAL "fj")
    message(FATAL_ERROR "Composite TV is not supported on the Fruit Jam platform")
endif()

# Composite-TV needs ~378 MHz: the TV scanline callback fires via a HW-alarm
# dispatcher at 30 kHz, and at 252 MHz the dispatch overhead alone consumes
# most of each ~32 us window (matches frank-cpc).
if(HDMI_DRIVER STREQUAL "COMPOSITE" AND CPU_SPEED LESS 300)
    message(STATUS "Overriding CPU_SPEED to 378 MHz for composite TV")
    set(CPU_SPEED "378" CACHE STRING "CPU clock in MHz" FORCE)
endif()

if(PLATFORM STREQUAL "z0")
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/boards CACHE PATH "" FORCE)
    set(PICO_BOARD "waveshare_rp2350_pizero" CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM "rp2350-arm-s" CACHE STRING "Pico platform" FORCE)
    if(CPU_SPEED GREATER "300")
        set(CPU_SPEED "252" CACHE STRING "CPU clock in MHz" FORCE)
    endif()
elseif(PLATFORM STREQUAL "fj")
    # The Pico SDK ships this board header (2.2.0 and later), so unlike the Z0
    # there is no PICO_BOARD_HEADER_DIRS override.  RP2350B in the 80-pin
    # package, so GPIO 0..47; the SDK header declares that with PICO_RP2350A 0.
    set(PICO_BOARD "adafruit_fruit_jam" CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM "rp2350-arm-s" CACHE STRING "Pico platform" FORCE)
    # The PIO HDMI timing is calibrated against 252 MHz, and there is no
    # composite driver here to want anything higher.
    if(CPU_SPEED GREATER "300")
        set(CPU_SPEED "252" CACHE STRING "CPU clock in MHz" FORCE)
    endif()
else()
    set(PICO_BOARD "pico2" CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM "rp2350-arm-s" CACHE STRING "Pico platform" FORCE)
endif()

if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 300)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

if(PLATFORM STREQUAL "m1")
    set(PLATFORM_DEF PLATFORM_M1)
    set(BOARD_DEF BOARD_M1)
    set(SD_CS 5)
    set(SD_SCK 2)
    set(SD_MOSI 3)
    set(SD_MISO 4)
    set(HDMI_PIN 6)
    set(TV_PIN 6)
elseif(PLATFORM STREQUAL "z0")
    set(PLATFORM_DEF PLATFORM_Z0)
    set(BOARD_DEF BOARD_Z0)
    set(SD_CS 43)
    set(SD_SCK 30)
    set(SD_MOSI 31)
    set(SD_MISO 40)
    set(HDMI_PIN 32)
elseif(PLATFORM STREQUAL "fj")
    # No TV_PIN: composite is rejected for this platform above, which is the
    # same reason the z0 branch omits it.
    # These four SD values are duplicated in src/board_fj.h and must agree;
    # drivers/sdcard/sdcard.c only ever sees this copy.
    set(PLATFORM_DEF PLATFORM_FJ)
    set(BOARD_DEF BOARD_FJ)
    set(SD_CS 39)
    set(SD_SCK 34)
    set(SD_MOSI 35)
    set(SD_MISO 36)
    set(HDMI_PIN 12)
else()
    # m2
    set(PLATFORM_DEF PLATFORM_M2)
    set(BOARD_DEF BOARD_M2)
    set(SD_CS 5)
    set(SD_SCK 6)
    set(SD_MOSI 7)
    set(SD_MISO 4)
    set(HDMI_PIN 12)
    set(TV_PIN 12)
endif()

# Video driver: HDMI_PIO_AUDIO is the target, VGA fallback via runtime detection
# (HDMI_DRIVER is declared near the top so COMPOSITE can adjust CPU_SPEED).

# Optional USB HID host (keyboard + gamepad).  When enabled, the native USB
# port is owned by the TinyUSB host stack, so USB-CDC stdio (serial console)
# and the boot-time USB-enumeration delay are disabled.  Off by default: the
# native USB port is then used for USB-CDC printf instead (PS/2 only input).
option(USB_HID_ENABLED "Enable USB HID host (keyboard + gamepad)" OFF)

# On the Fruit Jam the two USB-A sockets sit behind a CH334F hub whose upstream
# pair is on GPIO 1 and 2.  Those are ordinary PIO pins, not the RP2350's
# native USB controller, and the USB-C connector is wired as a device and
# cannot be made a host, so PIO-USB is the only input path the board has.
#
# Pico-PIO-USB claims one PIO block with three state machines: one for
# transmit, two for receive and end-of-packet.  Its transmit program has to sit
# at instruction offset 0 of that block.  So:
#
#   PIO0  video, three TMDS serialisers
#   PIO1  PIO-USB, three state machines, offset 0 free because this board has
#         no PS/2 socket and no NES pad connector
#   PIO2  I2S, once the codec driver exists
#
# PIO1 rather than PIO2 for USB is forced, not chosen: pio_usb.c maps its PIO
# index with a one-bit test (`num == 0 ? pio0 : pio1`), so PIO2 is not
# addressable from that library at the pinned version.
# The DMA channel is pinned rather than left at the library's default of 0.
# Pico-PIO-USB calls dma_claim_mask(), which panics on an already-claimed
# channel, and it initialises before the video driver.  The video driver claims
# dynamically and will simply skip whatever this took, but I2S hardcodes 10 and
# 11 and PWM hardcodes 8 and 9, so 7 is the highest channel that collides with
# nothing.
if(PLATFORM STREQUAL "fj" AND USB_HID_ENABLED)
    set(USB_HID_PIO_USB ON)
    set(USB_HID_PIO_DP_PIN 1)
    set(USB_HID_PIO_INDEX 1)
    set(USB_HID_PIO_DMA_CH 7)
endif()

message(STATUS "frank-micro  Platform: ${PLATFORM}  CPU: ${CPU_SPEED} MHz  Flash: ${FLASH_SPEED} MHz  Voltage: ${CPU_VOLTAGE}  HDMI: ${HDMI_DRIVER}  USB_HID: ${USB_HID_ENABLED}")

# Pico-PIO-USB, pinned as a submodule at lib/Pico-PIO-USB.  The Pico SDK's own
# TinyUSB supplies all the build glue for it: hw/bsp/rp2040/family.cmake defines
# a tinyusb_pico_pio_usb target that compiles the library, generates its two PIO
# headers and adds hcd_pio_usb.c to the host stack.  It looks for the sources at
# PICO_PIO_USB_PATH, so that has to be set before pico_sdk_init() runs, because
# that is when the SDK processes its tinyusb directory.
#
# The submodule must be a version with RP2350 support.  The 0.6.1 that TinyUSB
# 0.18.0 names in tools/get_deps.py does NOT have it, and without the RP2350-E9
# workaround in pio_usb_bus_get_line_state() the host never detects a device on
# the port: the board boots and displays perfectly and the keyboard is simply
# dead.  Pinned at 5a37a66, which compiles clean against TinyUSB 0.18.0's
# hcd_pio_usb.c.  See docs/HANDOFF.md before moving it.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB/src/pio_usb.c)
    set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB)
elseif(USB_HID_PIO_USB)
    message(FATAL_ERROR
        "PLATFORM=fj with USB_HID=1 needs the Pico-PIO-USB submodule. Run ./tools/setup.sh")
endif()

include(pico_sdk_import.cmake)

project(frank-micro C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" FRANK_MICRO_VERSION_RAW)
string(STRIP "${FRANK_MICRO_VERSION_RAW}" FRANK_MICRO_VERSION_RAW)
string(REPLACE " " "." FRANK_MICRO_VERSION_STR "${FRANK_MICRO_VERSION_RAW}")

add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/ps2)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/usbhid)

target_compile_definitions(ps2_driver PRIVATE ${PLATFORM_DEF}=1)

if(HDMI_DRIVER STREQUAL "HDMI_PIO_AUDIO")
    if(CPU_SPEED GREATER 252)
        set(FRANK_HDMI_SM_CLKDIV "2" CACHE STRING "" FORCE)
    else()
        set(FRANK_HDMI_SM_CLKDIV "1" CACHE STRING "" FORCE)
    endif()
    add_subdirectory(frank-hdmi-sound/src)

    # Use the STANDARD 32000 Hz HDMI audio rate (CEA-861).  Non-standard rates
    # such as the BBC SN76489's native 31250 Hz are mishandled by real HDMI
    # sinks (their audio clock does not lock to our Clock-Regeneration, so they
    # drop a sample every few seconds).  b-em produces at 31250 Hz and the
    # producer bridge (frank_audio.c) resamples to 32000 Hz.  For 32000 Hz the
    # default N=4096 gives CTS=25200 exactly.  (Matches the working frank-cpc.)
    target_compile_definitions(frank_hdmi_sound INTERFACE FRANK_HDMI_AUDIO_RATE=32000)

    add_library(drivers
        drivers/HDMI_audio.c
        drivers/HDMI_vga.c
        drivers/test_pins.c
        drivers/crash_handler.c
        drivers/tlv320dac3100.c
        drivers/audio.c
        drivers/audio_i2s_opt.S
        drivers/pwm_audio/pwm_audio.c
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )
    target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
    target_compile_definitions(drivers PUBLIC
        ${BOARD_DEF}
        ${PLATFORM_DEF}=1
        HDMI_PIO_AUDIO=1
    )
    target_compile_definitions(drivers PRIVATE
        PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    )
    pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)
    target_link_libraries(drivers
        pico_stdlib
        hardware_dma
        hardware_pio
        hardware_pwm
        hardware_spi
        hardware_gpio
        hardware_clocks
        hardware_irq
        hardware_vreg
        hardware_exception
        hardware_watchdog
        hardware_i2c
        frank_hdmi_sound
    )
elseif(HDMI_DRIVER STREQUAL "COMPOSITE")
    # --- COMPOSITE: software composite TV (PAL/NTSC) + I2S/PWM audio ---
    # TV engine (murmnes port).  Compiled with the symbol-rename and SRAM
    # memset/memcpy shims so it coexists with the HDMI.h API and the 30 kHz
    # scanline ISR never touches flash.
    add_library(tv_driver STATIC
        drivers/tv/tv-software.c
        drivers/tv/graphics.c
    )
    target_include_directories(tv_driver PUBLIC drivers drivers/tv)
    set(_tv_include_flags
        "-include ${CMAKE_CURRENT_SOURCE_DIR}/drivers/tv/tv_rename.h"
        "-include ${CMAKE_CURRENT_SOURCE_DIR}/drivers/tv/tv_mem.h"
    )
    string(JOIN " " _tv_include_flags_str ${_tv_include_flags})
    set_source_files_properties(
        drivers/tv/tv-software.c
        drivers/tv/graphics.c
        PROPERTIES
        COMPILE_FLAGS "${_tv_include_flags_str}"
    )
    target_compile_options(tv_driver PRIVATE
        -Wno-error
        -Wno-incompatible-pointer-types
        -Wno-all
        -fno-tree-loop-distribute-patterns
        -fno-builtin-memset
        -fno-builtin-memcpy
    )
    target_compile_definitions(tv_driver PUBLIC TV_BASE_PIN=${TV_PIN} ${PLATFORM_DEF}=1)
    target_link_libraries(tv_driver
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_pio
        hardware_clocks
    )

    add_library(drivers
        drivers/HDMI_tv.c
        drivers/crash_handler.c
        drivers/tlv320dac3100.c
        drivers/audio.c
        drivers/audio_i2s_opt.S
        drivers/pwm_audio/pwm_audio.c
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )
    target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
    target_compile_definitions(drivers PUBLIC
        ${BOARD_DEF}
        ${PLATFORM_DEF}=1
        VIDEO_COMPOSITE=1
    )
    target_compile_definitions(drivers PRIVATE
        PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    )
    pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)
    target_link_libraries(drivers
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_pio
        hardware_pwm
        hardware_spi
        hardware_gpio
        hardware_clocks
        hardware_irq
        hardware_vreg
        hardware_exception
        hardware_watchdog
        hardware_i2c
        tv_driver
    )
else()
    # HDMI_PIO: original PIO HDMI driver + I2S audio
    add_library(drivers
        drivers/HDMI.c
        drivers/hdmi_scanline.S
        drivers/HDMI_vga.c
        drivers/test_pins.c
        drivers/crash_handler.c
        drivers/tlv320dac3100.c
        drivers/audio.c
        drivers/audio_i2s_opt.S
        drivers/pwm_audio/pwm_audio.c
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )
    target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
    target_compile_definitions(drivers PUBLIC
        ${BOARD_DEF}
        ${PLATFORM_DEF}=1
        HDMI_PIO=1
    )
    target_compile_definitions(drivers PRIVATE
        PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    )
    pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)
    target_link_libraries(drivers
        pico_stdlib
        hardware_dma
        hardware_pio
        hardware_pwm
        hardware_spi
        hardware_gpio
        hardware_clocks
        hardware_irq
        hardware_vreg
        hardware_exception
        hardware_watchdog
        hardware_i2c
    )
endif()

set_source_files_properties(drivers/dlmalloc.c PROPERTIES COMPILE_DEFINITIONS
    "ONLY_MSPACES=1;HAVE_MMAP=0;HAVE_MORECORE=0;NO_MALLOC_STATS=1;USE_LOCKS=0;ABORT_ON_ASSERT_FAILURE=0;MALLOC_FAILURE_ACTION=;LACKS_SYS_MMAN_H=1;LACKS_UNISTD_H=1;LACKS_SYS_PARAM_H=1")

# ── BBC Micro (b-em) engine sources ────────────────────────────────────────
set(BEM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/bem)

# Use the hand-written Thumb-assembly 6502 + hw_event scheduler (fast path).
# Set to OFF to fall back to the portable C interpreter (src/bem/6502.c).
option(USE_THUMB_CPU "Use thumb_cpu asm 6502 + hw_event scheduler" ON)

set(THUMB_DIR ${BEM_DIR}/thumb_cpu)
set(THUMB_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/thumb_gen)

if(USE_THUMB_CPU)
    file(MAKE_DIRECTORY ${THUMB_GEN_DIR})
    find_program(RUBY_EXECUTABLE ruby REQUIRED)
    add_custom_command(
        OUTPUT ${THUMB_GEN_DIR}/6502_asm.inl ${THUMB_GEN_DIR}/6502_c.inl
        DEPENDS ${THUMB_DIR}/gen/gen_asm.rb
        WORKING_DIRECTORY ${THUMB_DIR}
        COMMAND ${RUBY_EXECUTABLE} gen/gen_asm.rb
                ${THUMB_GEN_DIR}/6502_asm.inl ${THUMB_GEN_DIR}/6502_c.inl
        COMMENT "Generating thumb_cpu 6502 tables"
        VERBATIM
    )
    set(CPU_SOURCES
        ${THUMB_DIR}/src/cpu/cpu.c
        ${THUMB_DIR}/src/cpu/cpu_asm.S
        ${THUMB_DIR}/src/cpu_mem.c
        ${THUMB_DIR}/src/adapter.c
        ${THUMB_GEN_DIR}/6502_asm.inl
        ${THUMB_GEN_DIR}/6502_c.inl
    )
else()
    set(CPU_SOURCES ${BEM_DIR}/6502.c)
endif()

set(BBC_ENGINE
    # 6502 CPU (thumb asm + hw_event, or portable C interpreter)
    ${CPU_SOURCES}
    # b-em core (subset; unused features compiled out via NO_USE_* guards)
    ${BEM_DIR}/acia.c
    ${BEM_DIR}/adc.c
    ${BEM_DIR}/cmos.c
    ${BEM_DIR}/compactcmos.c
    ${BEM_DIR}/compact_joystick.c
    ${BEM_DIR}/compat_wrappers.c
    ${BEM_DIR}/config.c
    ${BEM_DIR}/csw.c
    ${BEM_DIR}/ddnoise.c
    ${BEM_DIR}/disc.c
    ${BEM_DIR}/fdi.c
    ${BEM_DIR}/fdi2raw.c
    ${BEM_DIR}/i8271.c
    ${BEM_DIR}/ide.c
    ${BEM_DIR}/joystick.c
    ${BEM_DIR}/keyboard.c
    ${BEM_DIR}/logging.c
    ${BEM_DIR}/main.c
    ${BEM_DIR}/mem.c
    ${BEM_DIR}/model.c
    ${BEM_DIR}/mouse.c
    ${BEM_DIR}/music2000.c
    ${BEM_DIR}/music4000.c
    ${BEM_DIR}/music5000.c
    ${BEM_DIR}/pal.c
    ${BEM_DIR}/scsi.c
    ${BEM_DIR}/sdf-acc.c
    ${BEM_DIR}/sdf-geo.c
    ${BEM_DIR}/serial.c
    ${BEM_DIR}/sn76489.c
    ${BEM_DIR}/sysacia.c
    ${BEM_DIR}/sysvia.c
    ${BEM_DIR}/tape.c
    ${BEM_DIR}/tapenoise.c
    ${BEM_DIR}/uef.c
    ${BEM_DIR}/uservia.c
    ${BEM_DIR}/via.c
    ${BEM_DIR}/wd1770.c
    ${BEM_DIR}/win.c
    # linux.c omitted: host filesystem helpers not used on pico
    # pico glue: CRTC/ULA timing + rasteriser (X_GUI raw-row path)
    ${BEM_DIR}/pico/video/video.c
    ${BEM_DIR}/pico/video/display.c
    ${BEM_DIR}/pico/video/pixels.S
    ${BEM_DIR}/pico/video/gui_stub.c
    ${BEM_DIR}/pico/audio/audio.c
    ${BEM_DIR}/pico/sector_read/sector_read.c
    ${BEM_DIR}/pico/stub_allegro5/al_stub.cpp
    # embedded ROMs (Model B: os12, basic2, dfs226)
    ${BEM_DIR}/pico/roms/embedded_roms.c
)

set(JIT_STUBS)

# ── frank port layer (b-em glue) ────────────────────────────────────────────
set(PICO_PORT
    src/frank/frank_platform.c   # main(), board/clock/SD init, run loop
    src/frank/frank_gui.c        # x_gui_* hooks: 16bpp rows -> 8bpp HDMI fb
    src/frank/frank_keyboard.c   # PS/2 -> b-em key matrix
    src/frank/frank_audio.c      # sn76489 -> frank HDMI audio ring
    src/frank/frank_stubs.c      # host-symbol stubs (midi, wav, garbage mem)
    src/frank/frank_disc.c       # load SSD from SD card -> b-em disc
    src/frank/frank_console.c    # USB-serial key injection (headless control)
    src/frank/frank_settings.c   # runtime settings store + INI persistence
    src/frank/frank_loader.c     # SD disc image browser
    src/frank/frank_ui.c         # F11 media browser + F12 settings overlay
    src/frank/frank_screenshot.c # PrintScreen / F12 → BMP screenshot to SD
    src/Pico/ui_draw.c           # 8bpp framebuffer drawing primitives
    src/Pico/ui_font.c           # 6x8 bitmap font for the overlay
)

add_executable(frank-micro
    ${BBC_ENGINE}
    ${JIT_STUBS}
    ${PICO_PORT}
)

target_include_directories(frank-micro PRIVATE
    src
    src/Pico
    src/frank
    src/frank/vendor
    src/bem
    src/bem/pico
    src/bem/pico/video
    src/bem/pico/audio
    src/bem/pico/x_gui
    src/bem/pico/stub_allegro5/include
    src/bem/pico/roms
    src/bem/pico/sector_read
    src/bem/thumb_cpu/src
    src/bem/thumb_cpu/src/cpu
    drivers
    drivers/pwm_audio
    drivers/fatfs
    drivers/sdcard
    drivers/ps2
    drivers/nespad
)

if(HDMI_DRIVER STREQUAL "HDMI_PIO_AUDIO")
    target_include_directories(frank-micro PRIVATE frank-hdmi-sound/src)
endif()

target_compile_definitions(frank-micro PRIVATE
    ${BOARD_DEF}
    ${PLATFORM_DEF}=1
    PICO_BUILD=1
    PICO_ON_DEVICE=1
    FRANK_PERF_TIMING_HACKS=1
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
    LSB_FIRST=1
    FRANK_MICRO_VERSION=\"${FRANK_MICRO_VERSION_STR}\"
    SDCARD_PIN_SPI0_CS=${SD_CS}
    SDCARD_PIN_SPI0_SCK=${SD_SCK}
    SDCARD_PIN_SPI0_MOSI=${SD_MOSI}
    SDCARD_PIN_SPI0_MISO=${SD_MISO}
    # ── b-em build config ───────────────────────────────────────────────
    BEM
    USE_MEMORY_POINTER
    HAVE_STPCPY
    VERSION=\"frank-micro\"
    X_GUI                 # use x_gui.h raw 16bpp row abstraction (not scanvideo)
    SINGLE_CORE           # core1 is the frank HDMI encoder; no core1 video
    FRANK_MAIN            # disable al_stub main(); frank provides main()
    MODEL_MASTER          # boot as BBC Master 128 by default
    NO_USE_KEYCODE_MAP
    # feature trims (match kilograham pico defaults)
    NO_USE_TUBE NO_USE_DEBUGGER NO_USE_SID NO_USE_SAVE_STATE NO_USE_VDFS
    NO_USE_UEF NO_USE_CSW NO_USE_MMB NO_USE_IDE NO_USE_ADC NO_USE_MOUSE
    NO_USE_MUSIC5000 NO_USE_SCSI NO_USE_FDI NO_USE_I8271 NO_USE_ALLEGRO_GUI
    NO_USE_JOYSTICK NO_USE_PAL NO_USE_COMPACT NO_USE_CMD_LINE
    NO_USE_SOUND_FILTER NO_USE_SET_SPEED NO_USE_NULA_ATTRIBUTE
    NO_USE_NULA_PIXEL_HSCROLL NO_USE_DD_NOISE NO_USE_WRITABLE_CONFIG
    NO_USE_CLOSE NO_USE_CMOS_SAVE NO_USE_RAM_ROMS NO_USE_MIDI
    # rasteriser options (match kilograham)
    TILE8 MODE7_TILE8 USE_CRTC0_0_OPTIMIZATION LOOK_MA_NO_MA
    USE_MEM_DDNOISE USE_DDNOISE_8
    NO_USE_KEY_LOOKUP NO_USE_DISC_WRITE SHORTCIRCUIT_KEYBOARD_POLL
    USE_SECTOR_READ       # disc images streamed sector-by-sector from SD
    # -DFRANK_NO_AUTOBOOT=1 to skip the Prince of Persia disc autoboot
    $<$<BOOL:${FRANK_NO_AUTOBOOT}>:FRANK_NO_AUTOBOOT>
    PIOC_PRINTF_SUPPORT_FLOAT=0 PIOC_PRINTF_SUPPORT_LONG_LONG=0
)

if(USE_THUMB_CPU)
    # Thumb-assembly 6502 + event-driven hardware scheduler.
    target_compile_definitions(frank-micro PRIVATE
        USE_PICO_CPU
        USE_HW_EVENT
        THUMB_CPU_BEEB
        THUMB_CPU_USE_ASM=1
        CPU_ASM_INLINE_BREAKOUT
    )
    target_include_directories(frank-micro PRIVATE ${THUMB_GEN_DIR})
    target_link_libraries(frank-micro hardware_interp)
endif()

if(HDMI_DRIVER STREQUAL "HDMI_PIO_AUDIO")
    target_compile_definitions(frank-micro PRIVATE HDMI_PIO_AUDIO=1)
elseif(HDMI_DRIVER STREQUAL "COMPOSITE")
    target_compile_definitions(frank-micro PRIVATE VIDEO_COMPOSITE=1)
    target_link_libraries(frank-micro tv_driver)
endif()

if(USB_HID_ENABLED)
    target_include_directories(frank-micro PRIVATE drivers/usbhid)
endif()

if(PLATFORM STREQUAL "z0")
    target_compile_definitions(frank-micro PRIVATE SDCARD_SPI_BUS=spi1)
    target_link_libraries(frank-micro hardware_spi)
endif()

target_compile_options(frank-micro PRIVATE
    -O2
    -fno-strict-aliasing
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-missing-braces
    -Wno-implicit-fallthrough
    -Wno-pointer-sign
    -Wno-incompatible-pointer-types
    -Wno-char-subscripts
    -Wno-format
    -Wno-parentheses
)

# b-em sources include C++ (al_stub) — already enabled at project() level.

target_link_libraries(frank-micro
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_irq
    hardware_vreg
    hardware_clocks
    sdcard
    fatfs
    drivers
    ps2_driver
    nespad
)

if(USB_HID_ENABLED)
    target_link_libraries(frank-micro usbhid)
endif()

# On the Fruit Jam, keep a console that does not depend on USB at all.  The
# SDK board header puts UART1 on GPIO 8 and 9, both free on the 2x16 header, so
# a USB-serial adapter there gives printf and the serial console even in a
# USB_HID build, where the USB-CDC path is switched off below.  This matters
# more here than on the other boards: the Fruit Jam is the only hardware this
# project can test on, and a release build otherwise has no console at all.
if(PLATFORM STREQUAL "fj")
    pico_enable_stdio_uart(frank-micro 1)
else()
    pico_enable_stdio_uart(frank-micro 0)
endif()

# Dev builds (USB_HID off) use USB-CDC stdio for printf + the serial console;
# release builds (USB_HID on) disable it because the USB HID host needs
# exclusive use of the native USB controller.
#
# Note this coupling is a property of the other three boards, not of the Fruit
# Jam: there the USB-A ports hang off PIO and the native controller is unused,
# so the two could in principle coexist.  Doing so means running a TinyUSB
# device stack and a host stack in one binary, which is not attempted here; the
# UART console above is the answer instead.
if(USB_HID_ENABLED)
    pico_enable_stdio_usb(frank-micro 0)
else()
    pico_enable_stdio_usb(frank-micro 1)
endif()

# Never let a printf() to USB-CDC block Core 0: if the host has the port
# open but is not draining it, the default 500 ms blocking write stalls the
# audio producer and causes a recurring dropout.  0 = drop chars instead.
target_compile_definitions(frank-micro PRIVATE PICO_STDIO_USB_STDOUT_TIMEOUT_US=0)

pico_add_extra_outputs(frank-micro)
target_link_options(frank-micro PRIVATE -Xlinker --print-memory-usage --data-sections --function-sections)
