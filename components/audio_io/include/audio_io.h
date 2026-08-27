/*
 * osynth — audio I/O core.
 *
 * Owns the audio render task (pinned to core 1, or two of them across both
 * cores under the S45 pipeline below) and the output sinks.
 * Session 2: sink abstraction with real pacing — I2S (SYNTH_ENABLE_I2S_DAC),
 * classic-ESP32 internal DAC, or a timer-paced null sink. The sink's blocking
 * write is the audio clock.
 * Session 3: USB (UAC2) sink on ESP32-S3 — the S3 default unless I2S is
 * enabled; the host's iso polling paces the audio task while it streams.
 * Session 29: the two are no longer exclusive. With OSYNTH_USB_AUDIO_TAP the
 * I2S DAC keeps the clock and the USB interface is fed the same blocks as a
 * best-effort tap, so the synth records over USB while the DAC plays.
 * Session 31: audio also flows *in*. With OSYNTH_ENABLE_I2S_LINE_IN an I2S
 * ADC shares the DAC's port as a slave, so a captured block is sample-locked
 * to the block being played. The capture happens here, at the top of the
 * audio task; where it is mixed is the render chain's business, through the
 * three audio_io_line_in_* stages below.
 * Session 37: a second input device. With OSYNTH_ENABLE_MIC_IN a digital MEMS
 * microphone runs on the *second* I2S controller (source_mic.cpp) — a port has
 * one DIN pin and the line input has it. `in.source` chooses between them and,
 * since S37b, can take **both** at once: each device is captured into its own
 * buffer every block and the stages below run one mix pass per device, summing
 * in float. What stays shared is the placement and the trim — one `in.route`,
 * one `in.gain` — with `in.micgain` alongside for the level the two devices do
 * not have in common. Which is why those stages kept their names: what they
 * place in the render chain is "the input", and how many devices are behind it
 * is a question no consumer has to ask.
 * Session 45: on the P4 the chain runs as a two-core pipeline
 * (SYNTH_ENABLE_SPLIT_RENDER). audio_io_start_split() takes the chain in two
 * pieces instead of one and runs them as overlapping stages -- a voice stage
 * building block N+1, and a bus stage finishing block N with everything after
 * the voices plus the sink. Which core each lands on is decided in audio_io.cpp
 * and matters: see kVoiceCore there. Nothing below this line changes shape for
 * it -- the same render callback type, the same stages, the same stats. What
 * changes is that the capture is double-buffered, because the voice stage's
 * engines read the input through audio_io_in_mono() and
 * audio_io_line_in_block() while the bus stage is filling the next block into
 * the other slot -- see those two for the skew that buys.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "synth_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Renders one block of mono-per-channel float audio in [-1, 1].
 * Called from the audio task — no blocking, no allocation. Which core that is
 * depends on the build and, under the two-core pipeline, on which half of the
 * chain this callback is; no implementation of it should care. */
typedef void (*audio_render_fn)(float* out_l, float* out_r, size_t frames, void* ctx);

typedef struct {
    uint32_t blocks_rendered;
    uint32_t underruns;      /* blocks whose render ran past the block budget
                              * (deadline misses). Either core can raise one
                              * under the two-core pipeline, and a block that
                              * overran on both counts twice -- a distinction
                              * not worth a second counter, since by then the
                              * answer is the same in both directions. */
    float dsp_load_pct;      /* render time / block budget, EMA-smoothed.
                              * With the two-core pipeline this is the *worse*
                              * of the two cores rather than a sum: the deadline
                              * is per-core and per-block, so the core with the
                              * fuller block is the one that decides whether the
                              * next one arrives in time. Reading it as a total
                              * would make a pipeline about to fail look half
                              * loaded. The [voi fx loop] split beside it says
                              * which stage that is -- voi is the whole of the
                              * voice stage, the other two are the bus stage. */
    float dsp_load_peak_pct; /* worst single block since the previous
                              * audio_io_get_stats() call (reset on read) —
                              * for the per-engine CPU-budget figures (S8) */
    float out_peak;          /* loudest |sample| on the master bus *before*
                              * the soft clipper, since the previous
                              * audio_io_get_stats() call (reset on read).
                              * 1.0 = full scale; above that the signal is
                              * being saturated (S21) */
    uint32_t soft_clips;     /* cumulative samples (both channels counted)
                              * that entered the soft clipper's knee, i.e.
                              * ran above 0.8 of full scale. Climbing while
                              * out_peak stays near or above 1.0 means the
                              * patch is overloading the output — that is
                              * what an underrun-free click usually is,
                              * since those samples used to reach the int16
                              * hard clamp (S21) */
    /* Per-stage share of the block budget, EMA-smoothed like dsp_load_pct
     * (S21b). The render chain reports its three stages through
     * audio_io_report_stages(); the remainder of dsp_load_pct is the
     * audio task's own conversion + metering. Diagnostic only — this is
     * how you find out *which* stage is eating the budget instead of
     * guessing from the total. */
    float stage_voices_pct;
    float stage_fx_pct;
    float stage_loop_pct;
    /* Line input (S31); both stay 0 without SYNTH_ENABLE_LINE_IN.
     *
     * There is deliberately no fourth stage percentage to go with these: the
     * input costs a fixed ~0.7% that does not vary with anything the player
     * does, so it would never have told anyone anything. These two would. */
    float in_peak_l[2];   /* loudest |sample| captured since the previous
                           * audio_io_get_stats() call (reset on read),
                           * measured *before* in.gain — the ADC clips in the
                           * analogue domain and no firmware gain undoes it,
                           * so a post-gain meter would hide exactly the
                           * problem you need to see.
                           *
                           * Per channel, because a single combined peak
                           * cannot tell a quiet input from a dead channel:
                           * one silent side still gives a healthy reading
                           * from the other, which is precisely the case you
                           * reach for the meter to diagnose. */
    float in_peak_r[2];
    /* Peak of (L+R)/2 over the same window — the mono fold, measured rather
     * than assumed.
     *
     * Two healthy per-channel peaks say nothing about phase, and the looper
     * records folded whenever loop.mono is on (its default). A differentially
     * wired input whose legs land anti-phase on L and R therefore meters
     * perfectly, monitors perfectly through the stereo bus, and is annihilated
     * on the way into a take. That failure is invisible in in_peak_l/r and this
     * is the one number that shows it: roughly equal to them on a correlated
     * source, near zero on an anti-phase pair. Read-and-reset like the two
     * above. */
    float in_peak_mono[2];
    uint32_t in_starves[2]; /* cumulative blocks where the device had no full
                           * block ready; the tail was zero-filled. Should
                           * stay at 0 — a climbing count means the RX side
                           * is not clocking (MCLK, or the ADC's mode straps) */
    /* The route as the *audio task* resolved it, and the live smoothed gain at
     * each of the three mix points (mon, fx, dry — the kIn* order).
     *
     * Added because in_peak above cannot answer the question it kept raising:
     * a healthy peak with a silent-looking result says the ADC is fine and
     * says nothing about *where* the block is being mixed, and that is the
     * whole of what in.route decides. It is also the difference between an
     * input the looper records and one it cannot — the record tap sits between
     * fx/dry and mon (render_chain(), main.cpp) — so "heard but never in a
     * take" is exactly the symptom these three numbers separate from a
     * looper fault. Exactly one should be non-zero, and it should be the
     * position the control surface is showing; anything else means the write
     * never landed rather than that the tap missed it.
     *
     * Not read-and-reset: these are state, not a window. Both stay at their
     * defaults without SYNTH_ENABLE_LINE_IN. */
    uint8_t in_route;     /* 0 off, 1 mon, 2 fx, 3 dry */
    float in_g[3];        /* [kInMon], [kInFx], [kInDry] */
    /* Live smoothed gain per device, [0] line and [1] mic (S37b), matching
     * the peak arrays above. Both non-zero means both are being mixed —
     * either `in.source` is at `both`, or a crossfade between the two is
     * still running. Index [1] stays 0 on a build with no microphone.
     *
     * The mic's entry carries `in.micgain` folded in, so it is a mix
     * coefficient and not a flag: it is the number that says whether a mic
     * selected and metering healthily is actually reaching the bus at an
     * audible level, which is the one thing a peak beside it cannot. */
    float in_dev_g[2];
    /* Microphone only (S37d), and raw: every bit that appeared on each I2S slot
     * since the previous audio_io_get_stats() call, before narrowing and before
     * any gain. 0 on a build with no microphone.
     *
     * This is the meter for the question the peaks above cannot answer. They
     * are taken after the 24-to-16 truncation and printed as %.2f, so a
     * microphone 46 dB down and a data pin nobody drives both read 0.00. Here
     * the first is a small number and the second is exactly zero. */
    uint32_t mic_raw_or[2];
    /* Blocks the primary sink refused, cumulative, and the esp_err_t of the
     * most recent one (0 = none since boot).
     *
     * A counter rather than a log line, because the log line was on the audio
     * task. It was rate-limited to roughly one a second rather than latched,
     * so a sink that had stopped accepting blocks bought a fresh ~9 ms of
     * blocking console write every second — on top of whatever the fault was
     * already doing to the output. Reporting it here costs the audio task a
     * counter increment and moves the printing to the heartbeat, which is a
     * control task and can afford it. Not read-and-reset: this is a running
     * total, and what matters is whether it is climbing. */
    uint32_t sink_errors;
    int32_t sink_last_err;
    /* Waits the bus stage timed out on a voice stage that had not arrived
     * (S45); always 0 without the two-core pipeline.
     *
     * Distinct from `underruns` above, and the distinction is the whole
     * diagnosis. An underrun is one stage failing to finish inside its own
     * block period, which the per-stage percentages then localise -- an
     * over-budget voice stage lands there, not here, because it does deliver,
     * only late. A stall is the bus stage having nothing to work on at all: the
     * voice stage starved of scheduling rather than of budget. It presents
     * identically at the output while calling for the opposite fix -- not less
     * DSP, but finding whatever is holding that core off the CPU.
     *
     * Counted per timed-out wait rather than per block, so it climbs at the
     * timeout's rate while the condition lasts rather than at the block rate.
     * Non-zero at all is a fault, and the sink is emitting silence for as long
     * as it is climbing. */
    uint32_t pipe_stalls;
} audio_io_stats_t;

/* Called once per block from the render chain (audio task only) with the
 * cycle cost of each stage. No-op before the audio task computes its
 * budget. */
void audio_io_report_stages(uint32_t voices_cycles, uint32_t fx_cycles,
                            uint32_t loop_cycles);

/* The same three numbers, reported by whichever half of the chain measured
 * them, for the two-core pipeline where no single task sees all three.
 * audio_io_report_stages() above is these two called in order, so the single
 * and split paths feed one set of meters through one piece of arithmetic.
 *
 * Each is called from its own core and touches only its own fields, which is
 * what makes them safe without a lock: the EMA update is a read-modify-write,
 * and two cores doing that to one float would lose part of it. */
void audio_io_report_stage_voices(uint32_t voices_cycles);
void audio_io_report_stage_fx_loop(uint32_t fx_cycles, uint32_t loop_cycles);

/* Line input (S31), mixed into the render chain at one of three points — the
 * one `in.route` selects; the other two are silent and cost a compare each.
 * Audio-task only, and only meaningful after the capture at the top of the
 * block, which is why these are stages the render chain calls rather than a
 * widened render callback: it is the same shape the drum bus already uses
 * (drums_pre_fx / drums_post_fx), one capture with several possible
 * destinations.
 *
 * Which one is selected decides what gets recorded, because the looper's
 * record tap sits between them — see render_chain() in main.cpp:
 *   _fx   before the FX bus:  heard with effects, recorded with them
 *   _dry  after the FX bus:   heard dry, recorded dry
 *   _mon  after the looper:   heard dry, never recorded
 *
 * All three compile to nothing on builds without line-in. */
void audio_io_line_in_fx(float* l, float* r, size_t frames);
void audio_io_line_in_dry(float* l, float* r, size_t frames);
void audio_io_line_in_mon(float* l, float* r, size_t frames);

/* The block just captured, interleaved L/R int16 — SYNTH_BLOCK_SIZE frames, or
 * NULL when this build has no input or the RX half never came up.
 *
 * A fourth destination, and deliberately not a fourth stage: the modular
 * graph's LineIn node (S31f) is *inside* an engine, so it mixes the input at a
 * point only the patch knows, and it needs the samples rather than somewhere to
 * add them. Read-only, valid for the current block, audio task only.
 *
 * Independent of `in.route`, on purpose. That parameter names one of the three
 * bus mix points above; a patched-in node is a different question, and making
 * the two interact would mean either a node that goes silent when the route is
 * off or an input heard twice when it is not. So a graph patch works with
 * `in.route` at off, which is also how you would set it up.
 *
 * Its one caller is the graph's LineIn node, which is inside an engine and so
 * on the voice stage under the two-core pipeline. It therefore gets the same
 * previous-slot block audio_io_in_mono() describes above, for the same reason
 * and with the same 2.7 ms of extra age. The pointer stays valid for the whole
 * of that core's block. */
const int16_t* audio_io_line_in_block(void);

/* The current block as mono float in [-1, 1], summing every device `in.source`
 * selects, each at its own trim. Returns false and leaves `dst` untouched when
 * this build has no input or the RX half never came up; `dst` must hold at
 * least `frames` floats, and `frames` may not exceed SYNTH_BLOCK_SIZE. Audio
 * task only, valid for the current block.
 *
 * The difference from audio_io_line_in_block() above is the whole reason this
 * exists, and it is a difference in what the caller is asking for:
 *
 *   ..._line_in_block()  "the line input", one device, one wire. A modular
 *                        patch stores a node index, so following a selector
 *                        would make a saved patch sound different depending on
 *                        what someone later plugged into a jack it never
 *                        mentions.
 *   ..._in_mono()        "the audio input", whichever device the player has
 *                        told the synth to listen to. `in.source` is the one
 *                        global answer to that question, and a caller that
 *                        ignores it makes the unselected device unreachable —
 *                        which on a build with both is a microphone that
 *                        cannot be heard no matter what the player sets.
 *
 * Deliberately independent of `in.route` and `in.gain`, exactly as the graph
 * node is: those name the *monitor* path, and a caller here is a destination
 * of its own. Granulating the input while monitoring it dry, or while
 * monitoring nothing at all, both have to work. The per-device trim
 * (`in.micgain`) *is* applied, because without it `both` sums two devices that
 * arrive nowhere near each other in level.
 *
 * A MEMS mic at conversational distance sits far below full scale even after
 * that trim, so a caller that wants a usable signal from one should offer a
 * gain of its own rather than assume this arrives near unity.
 *
 * Under the two-core pipeline (S45) what "the current block" means depends on
 * which stage is asking, and both do: the granular engine and the sampler's
 * pre-roll sit on opposite sides of the cut. A caller on the bus stage -- the
 * FX bus, the sampler tap -- gets the block captured for the block being
 * finished, the same one the three mix stages used. A caller on the voice stage
 * -- an engine -- gets the previous slot instead, two block periods (2.7 ms)
 * older, because the bus stage is filling the fresh one while that engine runs.
 * Nothing is ever read while it is being written, and no caller has to know
 * which core it is on.
 *
 * The skew is real but it is not a phase error anyone can hear: it applies to
 * granulation and to sample pre-roll, neither of which is summed against the
 * monitored input. A caller that *did* need phase agreement with the bus would
 * have to be on the bus stage, and audio_io_in_fx_block() below is the entry
 * point for exactly that. */
bool audio_io_in_mono(float* dst, size_t frames);

/* The input exactly as audio_io_line_in_fx() mixed it into the bus this block,
 * written into `l` and `r` — overwritten, not accumulated. Returns false, and
 * leaves both untouched, when this build has no input, the RX half never came
 * up, or the fx position is silent. Both buffers must hold at least `frames`
 * floats, `frames` may not exceed SYNTH_BLOCK_SIZE, and the block is valid
 * only for the current one. Audio task only.
 *
 * This exists for one caller shape, and the shape is worth stating because it
 * is not the one the three stages above have: a *bus* unit that wants to
 * process the input alone and leave the synth beside it untouched — S39's two
 * noise-reduction units, whose whole point is cleaning a microphone without
 * putting a denoiser across the instrument. By the time the FX bus runs the
 * input has already been summed in, so such a unit cannot pull it back out.
 * It can, though, reproduce the exact block that was added, run its DSP on
 * that, and add the *difference* to the bus — which lands the same result and
 * needs no second mix point.
 *
 * The fx position and not a choice of the three, because it is the only one
 * already summed when the FX bus runs. `mon` and `dry` join afterwards, so
 * there is nothing on the bus to correct there: a caller gets false and should
 * do nothing rather than correct a signal that has not arrived.
 *
 * Follows `in.route`, `in.gain`, `in.source` and `in.micgain` down to the last
 * multiply — the opposite of audio_io_in_mono() above, which deliberately
 * ignores the first two. The difference is what each is for: that one names a
 * source to listen to, this one reproduces a mix that already happened, and a
 * correction that does not match what it is correcting is worse than none. */
bool audio_io_in_fx_block(float* l, float* r, size_t frames);

/* Picks and starts the output sink, then starts the audio task.
 * `render` may be NULL (silence). Falls back to the null sink (no output,
 * timer pacing) if the hardware sink fails to start. */
esp_err_t audio_io_start(audio_render_fn render, void* ctx);

#if SYNTH_ENABLE_SPLIT_RENDER
/* The same, with the chain handed over in two pieces to run as a two-core
 * pipeline (S45). `stage_a` must be the part of the chain with no reader on the
 * other side of the cut -- the voice manager, and nothing else. `stage_b` runs
 * with the sink, and gets the bus `stage_a` produced one block period earlier.
 * Which core each is pinned to, and why it is not arbitrary, is kVoiceCore in
 * audio_io.cpp.
 *
 * Both are called exactly once per block, in the same order and with the same
 * frame count as the single-core chain, so every ordering invariant the chain
 * documents still holds. What the caller pays for it is one block period of
 * added output latency and the requirement that the two halves really are
 * separable: a `stage_a` that reads anything `stage_b` wrote during the same
 * block gets the previous block's version of it.
 *
 * Either may be NULL (that half renders silence). Falls back to the null sink
 * exactly as audio_io_start() does -- a sink that fails does not collapse the
 * pipeline, since the null sink paces it just as well. */
esp_err_t audio_io_start_split(audio_render_fn stage_a, audio_render_fn stage_b,
                               void* ctx);
#endif

/* Name of the active sink: "i2s", "usb", "dac", "null" — or "none" before
 * start. A build that also taps a second destination reports both, primary
 * first: "i2s+usb-tap". */
const char* audio_io_sink_name(void);

void audio_io_get_stats(audio_io_stats_t* out);

/* Re-reads the microphone port's pads and logs what is moving (S37f).
 *
 * Exists as a public entry point for one reason: the same sweep runs inside
 * audio_io_start(), and at that moment an input codec has not been configured
 * yet — so its data pin standing still is the expected reading and not a
 * finding. Call this again once the codec is up, and the two lines together say
 * whether configuring it changed anything on the wire.
 *
 * A no-op on a build with no microphone. Boot-time only: it busy-polls. */
void audio_io_mic_probe_pads(void);

/* How long the master output has been inaudibly quiet, in milliseconds
 * (capped; 0 means it is currently making sound).
 *
 * Exists for one job: flash writes stall the render chain — ESP-IDF disables
 * the cache and parks the other core for the duration — so anything that
 * writes flash while the synth is sounding risks an underrun. Waiting for the
 * output itself to go quiet makes such a stall inaudible, and doing it on the
 * *output* rather than on "are any voices active" is what makes it correct:
 * a reverb tail, a delay repeat or a looper track is still sound, and a
 * voice-count check would happily write straight through one.
 *
 * Read from any control task. */
uint32_t audio_io_quiet_ms(void);

#ifdef __cplusplus
}
#endif
