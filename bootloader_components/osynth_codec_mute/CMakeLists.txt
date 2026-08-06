# Picked up automatically: the project's own project.cmake globs
# <project>/bootloader_components/* into the bootloader subproject.
#
# No REQUIRES. Everything early_mute.c uses is either a register header from
# soc (always available) or a ROM function — the bootloader has no drivers to
# depend on, which is the whole reason the I2C in there is bit-banged.
#
# The hooks it defines override weak symbols in the bootloader, so nothing
# references this component and the linker would drop it. early_mute.c defines
# bootloader_hooks_include() for exactly that reason; do not remove it.
idf_component_register(SRCS "early_mute.c")
