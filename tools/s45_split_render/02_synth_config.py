"""S45: add SYNTH_ENABLE_SPLIT_RENDER beside the render-placement macros."""
import io, sys

PATH = "components/synth_core/include/synth_config.h"
ANCHOR = """#ifdef CONFIG_OSYNTH_RENDER_IN_IRAM
#define SYNTH_RENDER_IRAM IRAM_ATTR
#else
#define SYNTH_RENDER_IRAM
#endif
"""

NEW = ANCHOR + '''
/* Two-core render pipeline (S45).
 *
 * The chain is cut in one place — after the voice manager — and the two halves
 * run as pipeline stages on separate cores: voices on core 0 producing block
 * N+1, everything after them (drum bus, FX bus, looper, sampler tap,
 * metronome) plus the sink on core 1 finishing block N. The stages overlap, so
 * the budget for one block period is roughly doubled.
 *
 * That cut point is not a preference. Every other candidate has state crossing
 * it inside a single block: drums_pre_fx() renders into a scratch that
 * drums_post_fx() and the FX bus compressor's key tap both read back, and the
 * three input mix positions have to agree with audio_io_in_fx_block() down to
 * the last multiply. Cutting after the voices leaves all of that whole on core
 * 1 and hands the other core a stage with one output and no readers.
 *
 * The capability gate is repeated here rather than left to Kconfig, the same
 * way SYNTH_ENABLE_USB_TAP's is, and for a reason that is real hardware rather
 * than defensiveness: a stage has one block period of slack, so anything that
 * stalls its core for longer is heard. On the P4 the BLE controller is on the
 * companion radio chip (SYNTH_BLE_VIA_HOSTED below), so core 0 sees only the
 * transport driver's interrupts. On the S3 the on-die controller runs
 * high-priority interrupts pinned to core 0 that no task priority can preempt,
 * which is exactly the stall this cannot absorb. */
#if defined(CONFIG_OSYNTH_SPLIT_RENDER) && defined(CONFIG_IDF_TARGET_ESP32P4)
#define SYNTH_ENABLE_SPLIT_RENDER 1
#else
#define SYNTH_ENABLE_SPLIT_RENDER 0
#endif
'''

src = io.open(PATH, encoding="utf-8", newline="").read()
if "SYNTH_ENABLE_SPLIT_RENDER" in src:
    print("already present; nothing to do")
    sys.exit(0)
if src.count(ANCHOR) != 1:
    sys.exit("anchor not unique: %d" % src.count(ANCHOR))
io.open(PATH, "w", encoding="utf-8", newline="").write(src.replace(ANCHOR, NEW))
print("inserted SYNTH_ENABLE_SPLIT_RENDER")
