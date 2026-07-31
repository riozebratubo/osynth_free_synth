/*
 * osynth — audio I/O core.
 *
 * Owns the audio render task (pinned to core 1) and the output sinks.
 * Session 2: sink abstraction with real pacing — I2S (SYNTH_ENABLE_I2S_DAC),
 * classic-ESP32 internal DAC, or a timer-paced null sink. The sink's blocking
 * write is the audio clock.
 * Session 3: USB (UAC2) sink on ESP32-S3 — the S3 default unless I2S is
 * enabled; the host's iso polling paces the audio task while it streams.
 * Session 29: the two are no longer exclusive. With OSYNTH_USB_AUDIO_TAP the
 * I2S DAC keeps the clock and the USB interface is fed the same blocks as a
 * best-effort tap, so the synth records over USB while the DAC plays.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Renders one block of mono-per-channel float audio in [-1, 1].
 * Called from the audio task on core 1 — no blocking, no allocation. */
typedef void (*audio_render_fn)(float* out_l, float* out_r, size_t frames, void* ctx);

typedef struct {
    uint32_t blocks_rendered;
    uint32_t underruns;      /* blocks whose render ran past the block budget
                              * (deadline misses) */
    float dsp_load_pct;      /* render time / block budget, EMA-smoothed */
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
} audio_io_stats_t;

/* Called once per block from the render chain (audio task only) with the
 * cycle cost of each stage. No-op before the audio task computes its
 * budget. */
void audio_io_report_stages(uint32_t voices_cycles, uint32_t fx_cycles,
                            uint32_t loop_cycles);

/* Picks and starts the output sink, then starts the audio task.
 * `render` may be NULL (silence). Falls back to the null sink (no output,
 * timer pacing) if the hardware sink fails to start. */
esp_err_t audio_io_start(audio_render_fn render, void* ctx);

/* Name of the active sink: "i2s", "usb", "dac", "null" — or "none" before
 * start. A build that also taps a second destination reports both, primary
 * first: "i2s+usb-tap". */
const char* audio_io_sink_name(void);

void audio_io_get_stats(audio_io_stats_t* out);

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
