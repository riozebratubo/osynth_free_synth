idf_component_register(
    SRCS "presets.cpp" "presets_factory.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core engines
    # persist: persist_owns(), the fence that keeps the working state and the
    # NVS settings from both owning one parameter. audio_io: audio_io_quiet_ms(),
    # which is how the working state's auto-save finds a moment where stalling
    # the render chain is inaudible.
    PRIV_REQUIRES seqarp fx drums graph persist audio_io
)
