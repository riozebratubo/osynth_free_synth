idf_component_register(
    SRCS "midi.c" "midi_serial.c"
    INCLUDE_DIRS "include"
    REQUIRES synth_core
    PRIV_REQUIRES usb_dev usb_host_midi esp_driver_uart engines fx drums
)
