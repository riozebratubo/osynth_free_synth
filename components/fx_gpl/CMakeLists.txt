# GPL-3 reverb algorithms — see include/fx_gpl.h and ./LICENSE.
#
# The component always registers so that fx.cpp can include fx_gpl.h
# unconditionally, but it contributes NO SOURCES unless the option is on.
# That is the whole licensing mechanism: with CONFIG_OSYNTH_FX_GPL unset
# there is no GPL object code in the link and the image stays MIT. The
# call sites in fx.cpp are behind SYNTH_ENABLE_FX_GPL, so nothing
# references the missing symbols either.
if(CONFIG_OSYNTH_FX_GPL)
    idf_component_register(
        SRCS "fx_gpl_mverb.cpp" "fx_gpl_dusk.cpp"
        INCLUDE_DIRS "include"
        REQUIRES synth_core
    )
else()
    idf_component_register(
        INCLUDE_DIRS "include"
        REQUIRES synth_core
    )
endif()
