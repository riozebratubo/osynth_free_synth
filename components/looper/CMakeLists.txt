idf_component_register(
    SRCS "looper.cpp" "loop_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core
    # loop_store (S16): esp_flash for the raw loops region; SDSPI + FAT for
    # the SD backend. All gated by Kconfig in code — unused paths compile
    # empty and link away.
    # seqarp: the beat clock behind loop.sync / loop.countin (S24).
    # drums:  the metronome click the count-in uses.
    PRIV_REQUIRES spi_flash fatfs sdmmc esp_driver_sdspi esp_driver_spi
                  seqarp drums
)
