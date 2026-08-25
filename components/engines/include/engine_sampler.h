/*
 * osynth — sampler engine (Session 44): public handle + parameter IDs.
 *
 * The sixth engine, and the only one whose sound is not synthesised: it plays
 * the pads of the currently selected sample kit (drums.h) from the keyboard.
 *
 * Two mappings, chosen by `smp.mode`:
 *
 *   pads     one pad per key, following the kit's own note map — the same
 *            map the MIDI router and the sequencer's drum lanes use, so a
 *            pad is in the same place whichever surface reaches for it. This
 *            is the default and it is the whole of what was asked for: the
 *            keyboard *becomes* the pads.
 *   pitched  one pad, played chromatically across the whole keyboard against
 *            `smp.root`. The same machinery, aimed differently: this is what
 *            turns a two-second recording of anything into an instrument.
 *
 * What this engine deliberately does not have is a filter, an LFO or a mod
 * matrix. Partly because the FX bus is already downstream of it and does that
 * job better, and partly for a reason worth writing down: the P4's `sram_low`
 * region — IRAM plus initialised data — had about 5 KB free when this was
 * written, and every kernel a fixed engine inlines lands in it. A sampler's
 * inner loop is two table reads, a lerp and an envelope multiply; a filter
 * family and two LFOs would have cost several times the engine itself. If that
 * headroom ever grows, the S33 filter block is the first thing to add here.
 *
 * The pads it plays belong to the drum bus, which can republish one under a
 * sounding voice — a pad being recorded over, erased or undone. A voice
 * therefore latches drums_kit_generation() alongside its sample pointer and
 * drops itself the moment the two disagree; two render boundaries later the
 * block behind it is safe to release. That handshake is the whole contract
 * between this engine and components/drums, and it is documented at
 * drums_kit_generation().
 */
#pragma once

#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const synth_engine_t g_engine_sampler;

/* Engine-specific range (0x02xx), registered by init() and dropped by
 * deinit() on an engine switch. Names, ranges and defaults in
 * docs/PARAM_MAP.md. */
#define SMPE_PID_MODE      0x0200 /* enum pads | pitched                    */
#define SMPE_PID_PAD       0x0201 /* int, which pad `pitched` plays         */
#define SMPE_PID_ROOT      0x0202 /* int, the key that plays it at 1x       */
#define SMPE_PID_START     0x0203 /* float, extra start offset on every pad */
#define SMPE_PID_VELDEPTH  0x0204 /* float, how much velocity shapes level  */
#define SMPE_PID_LEVEL     0x0205 /* float, engine output trim              */
#define SMPE_PID_SPREAD    0x0206 /* float, how far pad pan is honoured     */
#define SMPE_PID_ENV1_ATTACK  0x0207
#define SMPE_PID_ENV1_DECAY   0x0208
#define SMPE_PID_ENV1_SUSTAIN 0x0209
#define SMPE_PID_ENV1_RELEASE 0x020A

#ifdef __cplusplus
}
#endif
