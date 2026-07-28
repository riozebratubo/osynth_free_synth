# USB composite device (TinyUSB) — ESP32-S3 only. On the classic ESP32 (no
# USB-OTG peripheral) only the stub compiles and the tinyusb dependency is
# skipped entirely (idf_component.yml rules), so gate on the CMake target.
if(IDF_TARGET STREQUAL "esp32s3")
    # WHOLE_ARCHIVE: usb_descriptors.c defines the tud_descriptor_*_cb symbols
    # whose only references live in tinyusb's usbd.c — an archive the linker
    # processes *after* this one (dependents link before dependencies, no
    # --start-group). Without it the member is never pulled in and the link
    # fails with undefined tud_descriptor_*_cb.
    idf_component_register(
        SRCS "usb_dev.c" "usb_descriptors.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "tusb"
        PRIV_REQUIRES synth_core
        WHOLE_ARCHIVE
    )

    # TinyUSB compiles its class drivers against the application-provided
    # tusb_config.h; inject our config directory into the managed component's
    # library (same mechanism as esp-iot-solution's usb_device_uac).
    idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
    target_include_directories(${tusb_lib} PRIVATE "${COMPONENT_DIR}/tusb")
else()
    idf_component_register(
        SRCS "usb_dev.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES synth_core
    )
endif()
