idf_component_register(
    SRCS "codec_es8388.cpp" "codec_es8311.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core
    PRIV_REQUIRES esp_driver_i2c
)
