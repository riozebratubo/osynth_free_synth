idf_component_register(
    SRCS "fx.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core
    # S34. drums:  the compressor's sidechain key reads which slot sounded.
    #      seqarp: the live tempo, for note-division delay and LFO rates.
    # Private on both counts — nothing in fx.h exposes either.
    #
    # This closes the project's first dependency cycle: seqarp REQUIRES midi
    # (publicly), and midi PRIV_REQUIRES fx for its CC/NRPN map. ESP-IDF
    # supports circular component dependencies — every component library goes
    # on the link line inside one --start-group/--end-group, and requirement
    # expansion carries a visited set — so this resolves rather than looping.
    # If it ever does not, the cheap way out is a pair of function pointers
    # handed to fx by main.cpp instead of a direct dependency; the bus only
    # wants two scalars (seqarp_bpm, seqarp_beat_in_bar) out of seqarp.
    PRIV_REQUIRES drums seqarp
)
