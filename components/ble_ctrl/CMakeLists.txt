# What is left after the protocol moved out: the radio, and the two things the
# transport itself still reports on (the engine announcement's source and the
# USB role shown in the INFO/status paths).
set(priv_reqs bt esp_timer)

# Radio-less target: the NimBLE host still comes from `bt`, but its controller
# is on the companion chip and ESP-Hosted is what reaches it. Named with the
# managed-component prefix because that is how the component manager registers
# it. Guarded so the other targets neither require nor download it.
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND priv_reqs espressif__esp_hosted)
endif()

idf_component_register(
    SRCS "ble_ctrl.cpp"
    INCLUDE_DIRS "include"
    # ctrl_proto is public: this component is now a *transport* for it, and
    # ble_ctrl_init() installs itself there. Everything the handlers used to
    # reach directly is that component's business now, which is why the private
    # list below lost most of what it carried.
    REQUIRES synth_core ctrl_proto
    PRIV_REQUIRES ${priv_reqs}
)
