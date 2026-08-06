idf_component_register(
    SRCS "looper.cpp" "loop_store.cpp" "loop_stream.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core
    # loop_store (S16): esp_flash for the raw loops region; SDSPI + FAT for
    # the SD backend. loop_stream (S31) streams tracks off that same card and
    # shares its mount, so it needs no requirement of its own. All gated by
    # Kconfig in code — unused paths compile empty and link away.
    # esp_timer: the monotonic clock loop_store backs its re-mount attempts
    # off against — not implicitly available here, unlike freertos.
    # seqarp: the beat clock behind loop.sync / loop.countin (S24).
    # drums:  the metronome click the count-in uses.
    PRIV_REQUIRES spi_flash fatfs sdmmc esp_driver_sdspi esp_driver_spi
                  esp_timer seqarp drums
)
