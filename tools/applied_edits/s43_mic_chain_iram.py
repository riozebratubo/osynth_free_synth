#!/usr/bin/env python3
"""S43: move the microphone chain (vocoder, limiter) out of IRAM.

Applied edit, kept per the project's convention. fx.cpp has mixed line endings,
so every match is done line-ending-agnostically rather than on literal '\\n'.

Why: `sram_low` (175 KB) holds all IRAM code plus all initialised data, and a
linked HEAD image leaves only 5,408 bytes of it. See the note this inserts
above VocBand, and tools/iram_budget.py.
"""
import io
import re
import sys

PATH = 'components/fx/fx.cpp'


def replace_once(text, old, new):
    """Replace `old` with `new`, matching \\n against either \\n or \\r\\n."""
    pat = re.compile(r'\r?\n'.join(re.escape(x) for x in old.split('\n')))
    matches = list(pat.finditer(text))
    if len(matches) != 1:
        raise SystemExit('expected 1 match, found %d for: %r'
                         % (len(matches), old[:70]))
    m = matches[0]
    nl = '\r\n' if '\r\n' in m.group(0) else '\n'
    return text[:m.start()] + nl.join(new.split('\n')) + text[m.end():]


NOTE = '''/* ---- the microphone chain is not IRAM-resident (S43) ----
 *
 * vocoder_process() and limiter_process() below carry NOINLINE_ATTR and no
 * SYNTH_RENDER_IRAM, joining mnr_process()/mnr_frame() out in flash. Those
 * three are the units whose input is the microphone rather than the
 * instrument, and drawing the line there is what makes this a rule rather
 * than a series of individual retreats:
 *
 *   the instrument path - engines, drums, looper, and every effect that
 *                         colours them - keeps its flash-cache immunity;
 *   the microphone path - mic NR, vocoder, limiter - trades it for the IRAM
 *                         the instrument path needs.
 *
 * The measurements behind it. `sram_low` is 175 KB and holds all IRAM code
 * plus all *initialised* data - .bss lives in sram_high, and none of this is
 * guessable from the linker's errors; tools/iram_budget.py has the layout. A
 * linked image with none of this session's work leaves 5 408 bytes of it
 * free, and the mic NR alone is at or over that line, so this is not one unit
 * being slightly too big. These two free about 4.2 KB against a shortfall of
 * at most 960 bytes, which is the margin this should have had from the start.
 *
 * NOINLINE_ATTR is load-bearing, not decoration. Both are anonymous-namespace
 * functions with one call site, so GCC inlines them into fx_process() - which
 * *is* SYNTH_RENDER_IRAM - and the callee's own placement then means nothing.
 * Dropping SYNTH_RENDER_IRAM without noinline moves zero bytes and reports
 * the identical linker error, which is a mistake worth making only once.
 *
 * What it costs: with the vocoder or the limiter on, BLE or LittleFS traffic
 * can put a flash-cache miss inside the block. The budget is 1.33 ms and a
 * miss is microseconds - the same arithmetic the mic NR's note works through
 * to reach "should be inaudible" - but it is a real difference from the rest
 * of the bus, and these are the first units to suspect if something crackles
 * only while the app is talking. */

struct VocBand {'''


def main():
    text = io.open(PATH, encoding='utf-8', newline='').read()

    text = replace_once(
        text,
        '#include "esp_heap_caps.h"',
        '#include "esp_attr.h" /* NOINLINE_ATTR: the microphone chain, below */\n'
        '#include "esp_heap_caps.h"')

    text = replace_once(text, 'struct VocBand {', NOTE)

    text = replace_once(
        text,
        'void SYNTH_RENDER_IRAM vocoder_process(float* __restrict__ bl,\n'
        '                                       float* __restrict__ br, size_t frames) {',
        'NOINLINE_ATTR void vocoder_process(float* __restrict__ bl,\n'
        '                                   float* __restrict__ br, size_t frames) {')

    text = replace_once(
        text,
        'void SYNTH_RENDER_IRAM limiter_process(float* __restrict__ bl,\n'
        '                                       float* __restrict__ br, size_t frames) {',
        'NOINLINE_ATTR void limiter_process(float* __restrict__ bl,\n'
        '                                   float* __restrict__ br, size_t frames) {')

    text = replace_once(
        text,
        '/* Deliberately NOT SYNTH_RENDER_IRAM, unlike every other unit on this bus\n'
        ' * (S42). IRAM is full:',
        '/* Deliberately NOT SYNTH_RENDER_IRAM (S42); S43 drew the same line through\n'
        ' * the vocoder and the limiter, so see the microphone-chain note above\n'
        ' * VocBand for the rule the three now share. IRAM is full:')

    io.open(PATH, 'w', encoding='utf-8', newline='').write(text)
    print('applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
