"""S45 fix, part 2: the Kconfig help and the P4 defaults still described the
first (wrong) core assignment and argued the S3 exclusion from it.

Run from the repo root: python tools/s45_split_render/09_docs_after_swap.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _edit import Editor

k = Editor("components/synth_core/Kconfig.projbuild", skip_if="the empty core")
k.sub(
    """            Splits the render chain into two pipeline stages on separate
            cores: the voice manager on core 0, and everything after it —
            the drum bus, the FX bus, the looper, the sampler tap and the
            metronome — on core 1 alongside the sink. Core 0 renders block
            N+1 while core 1 finishes block N, so the two stages overlap
            and the per-block budget roughly doubles.""",
    """            Splits the render chain into two pipeline stages on separate
            cores: a voice stage, and a bus stage carrying everything
            after it — the drum bus, the FX bus, the looper, the sampler
            tap and the metronome — alongside the sink. The voice stage
            renders block N+1 while the bus stage finishes block N, so the
            two overlap and the per-block budget roughly doubles.

            How much that is worth depends on how the load actually
            divides, and it is rarely even. Measured on a granular patch it
            was 66% voices against 16% for the whole bus, so the ceiling
            was nearer 1.5x than 2x — and a patch whose voices alone
            exceed one core is not helped at all, because voices are a
            single stage and cannot be split further.

            The voice stage is pinned to the empty core (core 1) and the
            bus stage to core 0, where every task this firmware pins
            already lives. That is not arbitrary: a stage that goes over
            budget never blocks, so it holds its core at 100% until the
            patch gets cheaper. On core 0 that starves the BLE host, the
            sequencer clock and the idle task with it; on core 1 it costs
            only an idle task with nothing else to do. See kVoiceCore in
            components/audio_io/audio_io.cpp.""",
)
k.sub(
    """            P4 only, and that gate is real rather than cautious. The
            stage's slack is one block period, so anything that stalls
            core 0 for longer than that is heard. On the P4 the BLE
            controller lives on the companion radio chip
            (SYNTH_BLE_VIA_HOSTED), so core 0 sees only the transport
            driver's interrupts; on the S3 the on-die controller runs
            high-priority interrupts pinned to core 0 that no priority
            here can preempt. The S3 keeps the single-core chain, which
            is why render_chain() in main.cpp is still the composition of
            the same two stage bodies.""",
    """            P4 only, and that gate is real rather than cautious. The
            bus stage holds the sink, and therefore the hard deadline,
            while sharing core 0 with the radio. On the P4 the BLE
            controller lives on the companion chip (SYNTH_BLE_VIA_HOSTED),
            so that core sees only the transport driver's interrupts. On
            the S3 the on-die controller runs high-priority interrupts
            pinned to core 0 that no priority here can preempt, landing
            them on exactly the stage that must not miss a block. The S3
            keeps the single-core chain, which is why render_chain() in
            main.cpp is still the composition of the same two stage
            bodies.""",
)
k.save("help text follows the corrected assignment")

d = Editor("sdkconfig.defaults.esp32p4", skip_if="the empty core")
d.sub(
    """# Two-core render (S45): the voice manager on core 0, the drum bus, FX bus,
# looper, sampler tap and metronome plus the sink on core 1. The two stages
# overlap by one block, so the per-block budget is roughly doubled -- which is
# what pays for a fully loaded FX bus under a modular patch, the case
# graph_compile.h's reservation table explicitly cannot price.
#
# Set here rather than left at its Kconfig default because it is a P4-only
# decision with a P4-only reason. The stage's slack is one block period, so
# anything that parks a core for longer is heard, and on this target the BLE
# controller is on the companion C6 (CONFIG_BT_CONTROLLER_DISABLED above) --
# core 0 sees the hosted transport's interrupts and nothing worse. The S3 keeps
# the single-core chain because its on-die controller runs high-priority
# interrupts pinned to core 0 that no task priority can preempt.
#
# Costs one block (1.33 ms at 48 kHz / 64) of extra output latency, ~3 KB of
# .bss for the handover slots -- sram_high, not the sram_low region the IRAM
# budget fights over -- and a second 6 KB task stack.""",
    """# Two-core render (S45): a voice stage on the empty core (1), and a bus stage
# on core 0 carrying the drum bus, FX bus, looper, sampler tap and metronome
# plus the sink. The two overlap by one block, so the per-block budget is
# roughly doubled -- which is what pays for a fully loaded FX bus under a
# modular patch, the case graph_compile.h's reservation table explicitly cannot
# price.
#
# The assignment is the load-bearing part and it is argued at kVoiceCore in
# components/audio_io/audio_io.cpp: the voice stage is the heavy half and does
# not yield when it is over budget, so it belongs on the core nothing else is
# pinned to. Putting it on core 0 was tried first and starved the BLE host, the
# sequencer clock and the idle task the moment a granular patch went over.
#
# Set here rather than left at its Kconfig default because it is a P4-only
# decision with a P4-only reason: the bus stage holds the sink and shares core 0
# with the radio, and on this target the BLE controller is on the companion C6
# (CONFIG_BT_CONTROLLER_DISABLED above), so that core sees the hosted
# transport's interrupts and nothing worse. The S3 keeps the single-core chain
# because its on-die controller runs high-priority interrupts pinned to core 0
# that no task priority can preempt.
#
# Costs one block (1.33 ms at 48 kHz / 64) of extra output latency, ~3 KB of
# .bss for the handover slots -- sram_high, not the sram_low region the IRAM
# budget fights over -- and a second 6 KB task stack.""",
)
d.save("comment follows the corrected assignment")
