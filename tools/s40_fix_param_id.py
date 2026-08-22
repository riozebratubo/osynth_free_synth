p = 'components/presets/include/presets.h'
s = open(p, encoding='utf-8').read()
old = """#define PRESET_PID_STATE_RESET 0x0008"""
new = """#define PRESET_PID_STATE_RESET 0x000F"""
assert old in s
s = s.replace(old, new, 1)
s = s.replace(
    " * `state.reset` (0x0008) is the way back:",
    " * `state.reset` (0x000F) is the way back:", 1)
open(p, 'w', encoding='utf-8', newline='').write(s)

p = 'components/synth_core/include/synth_params.h'
s = open(p, encoding='utf-8').read()
old = """constexpr uint16_t PID_LINE_IN_MICGAIN = 0x000E;
"""
new = """constexpr uint16_t PID_LINE_IN_MICGAIN = 0x000E;
/* 0x000F is `state.reset`, the last of the 0x00xx globals in use — it belongs
 * to the preset system like the six triggers above it (PRESET_PID_STATE_RESET
 * in presets.h) and is named here only so this list stays the one place that
 * says which global ids are taken. */
"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
