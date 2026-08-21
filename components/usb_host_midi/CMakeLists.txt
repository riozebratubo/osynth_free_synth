# USB MIDI host (S35) — the other role of the OTG port, on the chips that have
# one. Same target gate as usb_dev: on the classic ESP32 only the stubs
# compile, and `usb_host` (IDF's host library) is not required at all. Keep
# this list in step with SYNTH_HAS_USB in synth_core/include/synth_config.h.
#
# usb_mode.cpp compiles everywhere — the role decision has to answer on every
# target, if only to say "device, there is nothing else here".
if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")
    # `espressif__usb`, not `usb`: since IDF 6.0 the USB Host Library is a
    # managed component (see idf_component.yml), and the component manager
    # registers it under its namespaced name.
    idf_component_register(
        SRCS "usb_host_midi.c" "usb_mode.cpp"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES synth_core espressif__usb
    )
else()
    idf_component_register(
        SRCS "usb_host_midi.c" "usb_mode.cpp"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES synth_core
    )
endif()
