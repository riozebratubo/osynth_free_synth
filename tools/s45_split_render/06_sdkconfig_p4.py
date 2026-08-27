"""S45: turn the two-core render pipeline on explicitly in the P4 defaults.

The Kconfig symbol already defaults on for this target, so this line changes
nothing about what is built. It is here because the file is where the target's
choices are argued rather than merely arrived at, and because a reader looking
for "why does the P4 render differently" should find it in the same place as
every other P4-only decision.

Run from the repo root: python tools/s45_split_render/06_sdkconfig_p4.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _edit import Editor

ed = Editor("sdkconfig.defaults.esp32p4", skip_if="OSYNTH_SPLIT_RENDER")
ed.sub(
    """# --- I2S analogue front end -------------------------------------------------""",
    """# --- Render pipeline --------------------------------------------------------

# Two-core render (S45): the voice manager on core 0, the drum bus, FX bus,
# looper, sampler tap and metronome plus the sink on core 1. The two stages
# overlap by one block, so the per-block budget is roughly doubled -- which is
# what pays for a fully loaded FX bus under a modular patch, the case
# graph_compile.h's reservation table explicitly cannot price.
#
# Set here rather than left at its Kconfig default because it is a P4-only
# decision with a P4-only reason. The stage's slack is one block period, so
# anything that parks a core for longer is heard, and on this target the BLE
# controller is on the companion C6 (CONFIG_BT_CONTROLLER_DISABLED below) --
# core 0 sees the hosted transport's interrupts and nothing worse. The S3 keeps
# the single-core chain because its on-die controller runs high-priority
# interrupts pinned to core 0 that no task priority can preempt.
#
# Costs one block (1.33 ms at 48 kHz / 64) of extra output latency, ~3 KB of
# .bss for the handover slots -- sram_high, not the sram_low region the IRAM
# budget fights over -- and a second 6 KB task stack.
#
# Turn it off to A/B a fault against the single-core chain without changing
# targets: render_chain() is the composition of the same two stage bodies, in
# the same order, so both paths render the same thing.
CONFIG_OSYNTH_SPLIT_RENDER=y

# --- I2S analogue front end -------------------------------------------------""",
)
ed.save("OSYNTH_SPLIT_RENDER=y")
