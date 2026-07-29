# The generated-header dir must exist at configure time: component.cmake
# validates PRIV_INCLUDE_DIRS before anything is built.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gen")

idf_component_register(
    SRCS "engines.cpp" "engine_subtractive.cpp" "engine_additive.cpp"
         "engine_fm.cpp" "engine_wavetable.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/gen"
    REQUIRES synth_core graph
)

# factory_wavetables.h is generated at build time by the host-side script
# (pure stdlib python — the IDF venv needs no extra packages). Regenerates
# only when the script changes.
idf_build_get_property(python PYTHON)
idf_build_get_property(project_dir PROJECT_DIR)
set(wt_header "${CMAKE_CURRENT_BINARY_DIR}/gen/factory_wavetables.h")
add_custom_command(
    OUTPUT "${wt_header}"
    COMMAND "${python}" "${project_dir}/tools/gen_wavetables.py" -o "${wt_header}"
    DEPENDS "${project_dir}/tools/gen_wavetables.py"
    COMMENT "Generating factory wavetables (tools/gen_wavetables.py)"
    VERBATIM
)
add_custom_target(gen_factory_wavetables DEPENDS "${wt_header}")
add_dependencies(${COMPONENT_LIB} gen_factory_wavetables)
