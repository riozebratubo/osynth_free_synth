set(srcs "audio_io.cpp" "sink_null.cpp" "sink_i2s.cpp" "sink_usb.cpp")
set(priv_reqs esp_timer esp_driver_i2s usb_dev)

# Internal DAC exists on the classic ESP32 only (see synth_config.h).
if(IDF_TARGET STREQUAL "esp32")
    list(APPEND srcs "sink_dac.cpp")
    list(APPEND priv_reqs esp_driver_dac)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES synth_core
    PRIV_REQUIRES ${priv_reqs}
)
