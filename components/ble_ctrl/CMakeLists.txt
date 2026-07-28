idf_component_register(
    SRCS "ble_ctrl.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core presets
    PRIV_REQUIRES bt engines midi seqarp drums esp_timer
)
