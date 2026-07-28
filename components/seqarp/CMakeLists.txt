idf_component_register(
    SRCS "seqarp.cpp" "seq_model.cpp" "seq_play.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core midi
    # drums: sequencer tracks can target the drum bus (S23).
    PRIV_REQUIRES esp_timer drums
)
