idf_component_register(
    SRCS "graph_model.cpp" "graph_compile.cpp" "graph_render.cpp"
         "graph_engine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core
)
