# SynthCtl v1 protocol, independent of transport (see include/ctrl_proto.h).
#
# Note what is NOT required here: `bt`. That is the point of the split -- this
# component speaks the protocol and knows nothing about how the bytes travel,
# so it builds on a host as readily as on a chip with a radio. ble_ctrl keeps
# the NimBLE dependency and registers itself as one transport.
#
# The requirement list is otherwise the one ble_ctrl used to carry, because the
# handlers are what reach into every subsystem: they are the app's way to touch
# the sequencer, the kit, the looper, the graph and the parameter store.
idf_component_register(
    SRCS "ctrl_proto.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core presets
    PRIV_REQUIRES engines midi seqarp chord drums graph looper persist
                  usb_host_midi esp_timer
)
