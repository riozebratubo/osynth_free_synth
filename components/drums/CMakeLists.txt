idf_component_register(
    SRCS "drums.cpp" "drum_kit.cpp"
    INCLUDE_DIRS "include"
    REQUIRES synth_core
    # esp_rom for the CRC used to validate kit images; the SD stack only for
    # the optional OSYNTH_DRUM_SD_KITS path (gated in code, links away when
    # the option is off).
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdspi esp_driver_spi
)

# ---------------------------------------------------------------------------
# Factory drum kit.
#
# The kit image is *not* generated during the firmware build: converting a
# WAV pack needs numpy/scipy, and the ESP-IDF python environment has neither
# (the same constraint that keeps tools/gen_wavetables.py on the standard
# library — but resampling 44.1 kHz drum takes in pure python is a different
# order of slow than synthesising a wavetable).
#
# So the pipeline is two steps, and only the second one is part of the build:
#
#   1. once, with your own python:
#        python tools/gen_drumkit.py --pack opendrums --out tools/out/drumkit.bin
#   2. every build: that image is embedded as .rodata and read in place
#      through the flash cache — the kit costs zero RAM.
#
# Point the build at a different image with -DOSYNTH_DRUMKIT=<path>. With no
# image present the build still links: an empty (but structurally valid) kit
# is generated instead — stdlib only — and the drum bus starts silent.
# ---------------------------------------------------------------------------
idf_build_get_property(python PYTHON)
idf_build_get_property(project_dir PROJECT_DIR)

if(NOT DEFINED OSYNTH_DRUMKIT)
    set(OSYNTH_DRUMKIT "${project_dir}/tools/out/drumkit.bin"
        CACHE FILEPATH "Drum kit image embedded into the firmware")
endif()

set(kit_bin "${CMAKE_CURRENT_BINARY_DIR}/drumkit.bin")

if(EXISTS "${OSYNTH_DRUMKIT}")
    file(SIZE "${OSYNTH_DRUMKIT}" _kit_size)
    math(EXPR _kit_kb "${_kit_size} / 1024")
    message(STATUS "osynth: embedding drum kit ${OSYNTH_DRUMKIT} (${_kit_kb} KB)")
    add_custom_command(
        OUTPUT "${kit_bin}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${OSYNTH_DRUMKIT}" "${kit_bin}"
        DEPENDS "${OSYNTH_DRUMKIT}"
        COMMENT "Embedding drum kit (${_kit_kb} KB)"
        VERBATIM
    )
else()
    message(STATUS
        "osynth: no drum kit at ${OSYNTH_DRUMKIT} — the drum bus will be "
        "silent. Build one with: python tools/gen_drumkit.py --pack <folder> "
        "--out tools/out/drumkit.bin")
    add_custom_command(
        OUTPUT "${kit_bin}"
        COMMAND "${python}" "${project_dir}/tools/gen_drumkit.py"
                --allow-missing
                --pack "${project_dir}/__osynth_no_pack__"
                --out "${kit_bin}"
        DEPENDS "${project_dir}/tools/gen_drumkit.py"
        COMMENT "Generating an empty drum kit (no sample pack configured)"
        VERBATIM
    )
endif()

# Produces _binary_drumkit_bin_start / _end, which drum_kit.cpp declares.
target_add_binary_data(${COMPONENT_LIB} "${kit_bin}" BINARY)
