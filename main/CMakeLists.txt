idf_component_register(
    SRCS "main.cpp"
    REQUIRES synth_core audio_io codec engines midi fx drums seqarp presets looper persist ble_ctrl usb_dev
             usb_host_midi local_ui nvs_flash esp_psram
)
