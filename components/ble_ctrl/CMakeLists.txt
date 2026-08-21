set(priv_reqs bt engines midi seqarp drums graph looper persist usb_host_midi esp_timer)

# Radio-less target: the NimBLE host still comes from `bt`, but its controller
# is on the companion chip and ESP-Hosted is what reaches it. Named with the
# managed-component prefix because that is how the component manager registers
# it. Guarded so the other targets neither require nor download it.
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND priv_reqs espressif__esp_hosted)
endif()

idf_component_register(
    SRCS "ble_ctrl.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core presets
    PRIV_REQUIRES ${priv_reqs}
)
