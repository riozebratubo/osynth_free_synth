idf_component_register(
    SRCS "fx.cpp" "fx_reverb_wet.cpp"
    INCLUDE_DIRS "include"
    # fx_gpl always registers but contributes no object files unless
    # CONFIG_OSYNTH_FX_GPL is set, so naming it here is free on an MIT
    # build and is what makes fx_gpl.h reachable on a GPL one. It cannot
    # cycle the way the PRIV_REQUIRES below does: fx_gpl depends only on
    # synth_core.
    REQUIRES synth_core fx_gpl
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
    # audio_io: the vocoder's modulator is the selected audio input (S38),
    #      read through audio_io_in_mono(); since S39b the two noise-reduction
    #      units read audio_io_in_fx_block() as well, to correct the input's
    #      contribution to the bus without touching the synth beside it.
    #      Private like the other two — nothing in fx.h exposes either call.
    PRIV_REQUIRES drums seqarp audio_io
)
