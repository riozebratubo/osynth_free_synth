p = 'README.md'
s = open(p, encoding='utf-8').read()

old = """  4 engines · 8 voices · 12 effects · 8×256-step sequencer
  16-slot drum kit · 8-track looper max 160s · USB audio + MIDI · BLE app
  presets + sequencer + looper persists
"""
new = """  4 engines · 8 voices · 12 effects · 8×256-step sequencer
  16-slot drum kit · 8-track looper max 160s · USB audio + MIDI · BLE app
  presets + sequencer + looper persists · powers on where you left off
"""
assert old in s
s = s.replace(old, new, 1)

old = """- **Module gating:** each engine declares the DSP blocks it uses; the rest are
  never allocated or processed
- **Presets:** 16 factory + 64 user slots per engine on LittleFS
"""
new = """- **Module gating:** each engine declares the DSP blocks it uses; the rest are
  never allocated or processed
- **Presets:** 16 factory + 64 user slots per engine on LittleFS
- **It powers on where you left off.** Nothing to press: whenever you stop
  touching it and the output goes quiet, the synth writes the engine, the
  patch, the drum kit, the modular graph and every sequencer pattern to flash,
  and reads them back at the next boot. Pull the plug mid-session and it comes
  back mid-session. Writes wait for silence and are skipped when nothing
  actually moved, so this costs no clicks and no wear. *Start from scratch* on
  the app's Home page is the way back to a blank instrument — it leaves your
  saved presets, your library and your volume and input settings alone.
"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
