"""S45: add the OSYNTH_SPLIT_RENDER Kconfig symbol.

Inserted ahead of OSYNTH_ENABLE_MODULAR, i.e. immediately after the
OSYNTH_RENDER_IN_IRAM block it belongs beside — both are knobs about how the
render path is placed rather than about what it renders.
"""
import io, sys

PATH = "components/synth_core/Kconfig.projbuild"
ANCHOR = "    config OSYNTH_ENABLE_MODULAR\n"

NEW = '''    config OSYNTH_SPLIT_RENDER
        bool "Render the chain as a two-core pipeline (S45)"
        depends on IDF_TARGET_ESP32P4
        default y if IDF_TARGET_ESP32P4
        default n
        help
            Splits the render chain into two pipeline stages on separate
            cores: the voice manager on core 0, and everything after it —
            the drum bus, the FX bus, the looper, the sampler tap and the
            metronome — on core 1 alongside the sink. Core 0 renders block
            N+1 while core 1 finishes block N, so the two stages overlap
            and the per-block budget roughly doubles.

            Costs one block (1.33 ms at the defaults) of extra output
            latency, and about 3 KB of internal RAM for the handover
            buffers. All of it lands in .bss, so none of it touches the
            P4's scarce sram_low region.

            P4 only, and that gate is real rather than cautious. The
            stage's slack is one block period, so anything that stalls
            core 0 for longer than that is heard. On the P4 the BLE
            controller lives on the companion radio chip
            (SYNTH_BLE_VIA_HOSTED), so core 0 sees only the transport
            driver's interrupts; on the S3 the on-die controller runs
            high-priority interrupts pinned to core 0 that no priority
            here can preempt. The S3 keeps the single-core chain, which
            is why render_chain() in main.cpp is still the composition of
            the same two stage bodies.

            Turn it off to A/B a fault against the single-core chain
            without changing targets: the chain order is defined once, so
            both paths render the same thing in the same sequence.

'''

src = io.open(PATH, encoding="utf-8", newline="").read()
if "OSYNTH_SPLIT_RENDER" in src:
    print("already present; nothing to do")
    sys.exit(0)
if src.count(ANCHOR) != 1:
    sys.exit("anchor not unique: %d" % src.count(ANCHOR))
io.open(PATH, "w", encoding="utf-8", newline="").write(src.replace(ANCHOR, NEW + ANCHOR))
print("inserted OSYNTH_SPLIT_RENDER")
