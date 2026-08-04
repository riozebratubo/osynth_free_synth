idf_component_register(
    SRCS "graph_model.cpp" "graph_compile.cpp" "graph_render.cpp"
         "graph_engine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core
    # audio_io: the LineIn node reads the captured input block (S31f). Private
    # — it appears in graph_render.cpp only, never in a public header.
    PRIV_REQUIRES audio_io
)
