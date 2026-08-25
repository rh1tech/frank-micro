#
# frank-micro — BBC Micro for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-micro
# SPDX-License-Identifier: GPL-3.0-or-later
#

# USB HID Driver for RP2350
# Provides keyboard and gamepad input via USB Host
#
# SPDX-License-Identifier: MIT

add_library(usbhid INTERFACE)

# Check if USB HID is enabled
if(USB_HID_ENABLED)
    message(STATUS "USB HID Host enabled")
    message(STATUS "  NOTE: USB CDC stdio will be DISABLED when USB HID is enabled!")
    
    # Add the actual source files when enabled
    target_sources(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/hid_app.c
        ${CMAKE_CURRENT_LIST_DIR}/xinput_host.c
        ${CMAKE_CURRENT_LIST_DIR}/usbhid_wrapper.c
    )

    # Expose both this dir (tusb_config.h + usbhid.h) and the parent dir
    # (usbhid_wrapper.h — also the stubbed header when USB_HID is off).
    target_include_directories(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/..
    )

    target_compile_definitions(usbhid INTERFACE
        USB_HID_ENABLED=1
    )
    
    # Link TinyUSB Host
    target_link_libraries(usbhid INTERFACE
        tinyusb_host
        tinyusb_board
    )
    
    # PIO-USB host, for boards whose USB-A ports hang off PIO pins rather than
    # the native USB controller.  Setting CFG_TUH_RPI_PIO_USB compiles
    # hcd_rp2040.c out entirely, so the PIO port becomes the only host
    # controller and the native controller is left free.
    if(USB_HID_PIO_USB)
        if(NOT TARGET tinyusb_pico_pio_usb)
            message(FATAL_ERROR
                "USB_HID_PIO_USB is set but the tinyusb_pico_pio_usb target does not "
                "exist. PICO_PIO_USB_PATH must point at a Pico-PIO-USB checkout "
                "before pico_sdk_init(). Run ./tools/setup.sh")
        endif()
        message(STATUS "USB HID using PIO-USB on GPIO${USB_HID_PIO_DP_PIN}")
        target_compile_definitions(usbhid INTERFACE
            CFG_TUH_RPI_PIO_USB=1
            USB_HOST_PIO_DP_PIN=${USB_HID_PIO_DP_PIN}
            USB_HOST_PIO_INDEX=${USB_HID_PIO_INDEX}
            USB_HOST_PIO_DMA_CH=${USB_HID_PIO_DMA_CH}
        )
        target_link_libraries(usbhid INTERFACE tinyusb_pico_pio_usb)
    endif()
else()
    message(STATUS "USB HID Host disabled (use -DUSB_HID_ENABLED=1 to enable)")
    message(STATUS "  USB CDC stdio remains available for debug output")
    # Stub functions are provided inline in drivers/usbhid_wrapper.h
    # Do NOT include the usbhid directory to avoid tusb_config conflicts
endif()
