#!/usr/bin/env python3
"""S37i - fold the P4's on-board mic to both channels.

With the mic finally working (S37h), it monitors on the left only: the port is
in OSYNTH_MIC_STEREO, which passes both raw slots through verbatim, and the
ES8311 is a mono codec whose ADC sits in the *left* slot (REG44 = 0x58, the
vendor's "ADCL + DACR"). Left carries the microphone, right carries this codec's
own DAC output, which osynth never feeds — so the right channel is silence and
the monitor path is hard-panned left.

That STEREO setting was a bring-up instrument, not a feature. It existed because
which slot a mono codec lands in was unknown, and reading the wrong one is
indistinguishable from a dead microphone — the exact failure this port kept
producing. The question is now answered twice over: by the vendor's register
semantics and by hearing it on the left.

So this switches to the mono path source_mic.cpp already has, which reads the
one live slot and writes it to *both* channels. Two things follow:

  - Monitoring is centred rather than hard left.
  - Mono looper takes stop losing 6 dB. The looper records (L+R)/2 when
    loop.mono is on, so a source living in one channel comes back half
    amplitude in every take while metering perfectly — the same trap the
    ES8388's differential mode set, and the reason the mono branch duplicates
    rather than pans.

Nothing diagnostic is lost: `raw xxxxxx/xxxxxx` in the heartbeat accumulates
both raw slots before this branch, so the per-slot evidence is still there.

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


D = u'—'

edit('sdkconfig.defaults.esp32p4', [(
    u'''# Both slots, not one {D} and here that is a diagnostic before it is a feature.
#
# The ES8311 is a **mono** codec: one ADC, one sample per frame, and which half
# of the I2S frame it lands in is a property of the chip's output routing
# (register 0x44) rather than of a strap this firmware can read. Taking one
# slot means a wrong guess is indistinguishable from a dead microphone {D}
# exactly the failure mode this port keeps producing. Taking both makes the
# heartbeat show them separately, so `mic 0.12/0.00` names the slot outright
# and `mic 0.00/0.00` clears the question entirely and points back at the codec.
#
# Worth leaving on afterwards regardless: on a mono source the empty channel
# costs one SIMD lane and the meters keep telling the truth about the routing.
CONFIG_OSYNTH_MIC_STEREO=y
'''.replace('{D}', D),
    u'''# One slot, folded to both channels (S37i).
#
# The ES8311 is a **mono** codec: one ADC, one sample per frame, and which half
# of the I2S frame it lands in is a property of the chip's output routing
# (register 0x44) rather than of a strap this firmware can read. That is why
# this was OSYNTH_MIC_STEREO through the bring-up {D} taking both slots made the
# heartbeat name the live one, where taking the wrong single slot would have
# been indistinguishable from the dead microphone this port kept producing.
#
# The answer is now in twice: REG44 = 0x58 is the vendor's "ADCL + DACR", so the
# ADC is in the **left** slot, and a working mic duly arrived hard left. So the
# instrument comes off and the mic reads as what it is.
#
# Two things follow from the mono path in source_mic.cpp, which reads the one
# live slot and writes it to *both* channels:
#
#   - Monitoring is centred instead of hard left. The other slot was never
#     silence-with-a-mic-in-it; it is this codec's own DAC output, which osynth
#     does not feed.
#   - Mono looper takes stop losing 6 dB. The looper records (L+R)/2 when
#     loop.mono is on, so a source living in one channel comes back at half
#     amplitude in every take while metering perfectly. Duplicating rather than
#     panning is what makes a mono source fold to itself {D} the same trap the
#     ES8388's differential input set, resolved the same way.
#
# No diagnostic is given up: `raw xxxxxx/xxxxxx` in the heartbeat accumulates
# both raw slots *before* this branch, so per-slot evidence survives. Put
# STEREO back if a stereo pair is ever wired to this port.
# CONFIG_OSYNTH_MIC_STEREO is not set
CONFIG_OSYNTH_MIC_SLOT_LEFT=y
'''.replace('{D}', D))])


# The commented example block further down contradicted the live settings once
# they were no longer commented out. Point it at them instead.
edit('sdkconfig.defaults.esp32p4', [(
    u'''# Slot the mic drives (its L/R strap), stereo pair instead, and digital gain
# in bits at the 24-to-16 truncation. See sdkconfig.defaults.esp32s3 for the
# full notes on each — they are target-independent. LEFT is the Kconfig
# default and matches L/R tied to GND; swapping it is the first thing to try
# on a mic that reads silence with the starve counter at 0.
# CONFIG_OSYNTH_MIC_SLOT_LEFT=y
# CONFIG_OSYNTH_MIC_STEREO=y
''',
    u'''# Slot the mic drives (its L/R strap), stereo pair instead, and digital gain
# in bits at the 24-to-16 truncation. See sdkconfig.defaults.esp32s3 for the
# full notes on each — they are target-independent. All three are set for real
# further up this file (SLOT_LEFT, STEREO off, SHIFT 3); this paragraph is here
# for the external-MEMS path, where LEFT matches an L/R pin tied to GND and
# swapping it is the first thing to try on a mic that reads silence with the
# starve counter at 0.
''')])

# The generated sdkconfig has to move too. `sdkconfig.defaults` only supplies
# symbols the existing sdkconfig does not already set, and this one is set — so
# editing the defaults alone would change nothing until the next fullclean, and
# would look exactly like the code not working. Guarded, because a tree that has
# never been configured has no sdkconfig to edit.
#
# No trailing newline in the anchor: the generated sdkconfig is CRLF and
# io.open(newline='') keeps it that way, so matching on '...=y\n' finds nothing.
# The bare symbol appears exactly once in that file.
import os
if os.path.exists('sdkconfig'):
    edit('sdkconfig', [(u'CONFIG_OSYNTH_MIC_STEREO=y',
                        u'# CONFIG_OSYNTH_MIC_STEREO is not set')])
else:
    print('no sdkconfig yet; defaults will apply on the next configure')

print('done')
