idf_component_register(
    SRCS "chord.cpp"
    INCLUDE_DIRS "include"
    # seqarp owns the scale tables (seq_scale_mask / seq_quantize) and the
    # seq.scale / seq.root ids that chord.follow mirrors — one owner of what
    # "dorian" means, rather than two copies free to disagree.
    #
    # The dependency runs chord -> seqarp -> midi and no further: the router
    # knows nothing about chord mode, which is what keeps this acyclic. See
    # midi_set_chord_hook() in midi.h.
    REQUIRES synth_core midi seqarp
    PRIV_REQUIRES esp_timer
)
