idf_component_register(
    SRCS "persist.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core
    # nvs_flash: the store itself. audio_io: audio_io_quiet_ms(), which is how
    # a write finds a moment where stalling the render chain is inaudible.
    PRIV_REQUIRES nvs_flash audio_io
)
