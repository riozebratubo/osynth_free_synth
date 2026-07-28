idf_component_register(
    SRCS "presets.cpp" "presets_factory.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core engines
    PRIV_REQUIRES seqarp fx drums
)
