#!/usr/bin/env python3
"""S37d docs - record the corrected APLL rule and what the P4 mic port now does.

Three claims in the prose went stale when the on-board microphone moved this
target from a shared-clock slave to a master on the board's own pins, and one
of them ("APLL is the DAC's and must not be retuned") was never true of a
master in the first place. See tools/s37d_patch_mic_diag.py for the code.

Kept per the project's artifacts rule.
"""
import io
import sys


def edit(path, subs):
    with io.open(path, encoding='utf-8', newline='') as f:
        src = f.read()
    for old, new in subs:
        if new in src:
            print('  (already applied) %r' % old[:50])
            continue
        n = src.count(old)
        if n != 1:
            sys.exit('%s: anchor count %d: %r' % (path, n, old[:70]))
        src = src.replace(old, new)
    with io.open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(src)
    print('patched %s' % path)


edit('PINMAP.md', [(
    u"""> bootloops — `APLL` is the DAC's and must not be retuned, and `EXTERNAL` needs
> a faster pin clock than the board has. What works is **XTAL with a divided
> declared rate**: a slave's declared rate never reaches a pin, so halving it
> halves the internal clock demand and changes nothing on the wire. The chain
> in `source_mic.cpp` picks it automatically and the boot log names it — expect
> `clk xtal, declared rate/2` here. Full write-up in
> `sdkconfig.defaults.esp32p4`.
""",
    u"""> bootloops — and `EXTERNAL` needs a faster pin clock than the board has. What
> works **for a slave** is **XTAL with a divided declared rate**: a slave's
> declared rate never reaches a pin, so halving it halves the internal clock
> demand and changes nothing on the wire. Expect `clk xtal, declared rate/2` on
> the shared-clock (external MEMS) path. Full write-up in
> `sdkconfig.defaults.esp32p4`.
>
> **A master takes APLL instead, and the ban on it was wrong** (S37d). It
> applied to a slave, whose divided declared rate really would ask for a
> different frequency; a master asks for `mclk = 256 x fs`, the identical figure
> `sink_i2s.cpp` asks for on the output port. And IDF cannot retune an occupied
> APLL in any case — `esp_clk_tree_src_set_freq_hz()` returns
> `ESP_ERR_INVALID_STATE` and hands back the frequency it is already running at.
> This matters because on a master that MCLK is a *pin*: from the 40 MHz XTAL,
> 12.288 MHz needs a fractional divider (3 + 49/192), so the clock an ES8311
> derives its whole ADC timing from dithers between 75 ns and 100 ns periods.
> Expect `clk apll` on the on-board-mic path — and, as a free consequence, a
> capture sample-locked to playback rather than in a second clock domain.
""")])

edit('sdkconfig.defaults.esp32p4', [(
    u"""#   APLL         would clear it, and is refused on purpose: asking APLL for a
#                frequency *sets* the one APLL on the chip, which is the clock
#                the DAC is running on.
""",
    u"""#   APLL         would clear it, and is skipped **for a slave only** — there
#                the declared rate is divided, so the request really would name
#                a frequency the DAC's does not. On a *master* it is the first
#                thing tried (S37d): the request is mclk = 256 x fs, the same
#                figure sink_i2s.cpp asks for, and IDF cannot retune an
#                occupied APLL anyway (esp_clk_tree_src_set_freq_hz returns
#                ESP_ERR_INVALID_STATE and reports the running frequency).
""")])

edit('sdkconfig.defaults.esp32p4', [(
    u"""# All of that is automatic — kClockAttempts in components/audio_io/source_mic.cpp
# walks XTAL, then XTAL /2, then /4, and the boot log names the one that took.
# Expect "clk xtal, declared rate/2" on this board. If the port opens but
# `in starve` climbs every block, raise OSYNTH_MIC_CLK_SKIP in that file by one.
""",
    u"""# All of that is automatic — kClockAttempts in components/audio_io/source_mic.cpp
# walks the candidates and the boot log names the one that took. Expect
# "clk xtal, declared rate/2" on the shared-clock (external MEMS) path and
# "clk apll" on the on-board-mic path, where this port masters its own pins and
# its MCLK is a real pin feeding a codec's clock manager — see the APLL row
# above. If the port opens but `in starve` climbs every block, raise
# OSYNTH_MIC_CLK_SKIP in that file by one.
""")])

edit('tools/_p4_board.txt', [(
    u"""ADC-only versus the board's own DAC-enabled pairing) were tried. All silent.
What remains untested needs the board schematic or its ESPHome `es8311`
component source.
""",
    u"""ADC-only versus the board's own DAC-enabled pairing) were tried. All silent.

**S37d picked it back up, and the useful move was to stop testing the codec.**
Everything above is about the register file; nothing had measured the two things
either side of it. What changed:

- **The port now masters on APLL, not XTAL.** From a 40 MHz XTAL, 12.288 MHz
  needs a fractional divider, so the MCLK on GPIO13 — the only clock an ES8311
  has, and the one its clock manager divides for the ADC — dithered between
  75 ns and 100 ns periods. The board's other codec has been on APLL since the
  P4 bring-up for exactly this reason. The old "never APLL" rule in
  `source_mic.cpp` was about a *slave*, whose divided declared rate really would
  ask for a different frequency; a master asks for the same 256 x fs the sink
  does, and IDF refuses to retune an occupied APLL regardless.
- **Raw-slot telemetry.** The heartbeat now prints `raw xxxxxx/xxxxxx`, the OR
  of every raw word's magnitude bits per slot per window, taken *before* the
  24-to-16 truncation and before any gain. This is what separates "the pin never
  left zero" from "the signal is 60 dB down" — the two readings that both print
  `mic 0.00/0.00`, and the ambiguity five rounds of register work were spent
  inside. `MIC_SHIFT` also went 0 -> 3 so the ordinary meter can see a quiet mic.
- **A GPIO matrix dump** of the four mic pads at boot, and the ES8311's chip-ID
  registers (0xFD/0xFE, expect 83 11) beside the register dump — so "the
  register file matches the vendor byte for byte" rests on reads of a part that
  has identified itself.

**Still unmeasured, and now the top of the list: do GPIO10/12/13 drive?** Nothing
has ever checked, and DMA filling at the right rate proves only that the
peripheral's internal clock runs — a master fills its ring whether or not a
single pad is connected. This board has form: 45/46/47 read 0.6 V and cost a
week. A multimeter on each while the synth runs settles it in two minutes; a
toggling pin reads about half of 3.3 V, a dead one reads 0 V, 3.3 V or 0.6 V.

**Also unresolved: where the analogue element actually is.** `microphone_type:
analog` says it is on the ES8311's preamp, not which pins — an on-board element
and the mic pin of a TRRS headset jack look identical from the firmware. Plugging
a headset in is a two-minute test of the second reading.
""")])

print('done')
