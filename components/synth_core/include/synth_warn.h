/*
 * osynth — deferred warnings from the render path.
 *
 * Why this exists: ESP_LOG from the audio task is a dropout, every time.
 * The console is a blocking 115200-baud UART, so a hundred-character line
 * takes 8.7 ms of busy-wait — against four I2S DMA buffers, 5.33 ms in total
 * (sink_i2s.cpp). The line always wins. On a build whose console is the USB
 * serial JTAG the number is different and the conclusion is not: the write is
 * still synchronous and still unbounded from the audio task's point of view.
 *
 * That made the render path's diagnostics self-defeating. Every one of them
 * fires on a condition the player is *already* investigating — a vocoder with
 * no input, a granular engine with no capture ring, a sink that stopped
 * accepting blocks — and each one added a click of its own to the symptom
 * being diagnosed. Worst was the sink-failure line, which was rate-limited
 * rather than latched and so bought a fresh stall every second for as long as
 * the fault lasted.
 *
 * So the audio task stores a pointer and moves on, and a control task does
 * the printing. Two rules make that safe:
 *
 *  - **Static strings only.** What is queued is the address of a literal, not
 *    a formatted buffer, so there is nothing to allocate, nothing to copy and
 *    no lifetime to get wrong. A warning that needs to interpolate a number
 *    is a warning that wants a counter in a stats struct instead — see
 *    audio_io_stats_t, which is how the sink's failures are reported now.
 *  - **Single producer, single consumer.** The audio task is the only writer
 *    and the drain runs on one control task, so the ring needs no lock and no
 *    critical section: a release store of the head publishes the slot, and
 *    the matching acquire load is what lets the reader see it.
 *
 * Dropping is the right failure mode when the ring is full. These are latched
 * one-shots — the same warning does not queue twice — so a full ring means
 * more distinct faults than slots, and the first few are the ones worth
 * having.
 */
#pragma once

namespace osynth::dsp {

/* Queue one warning. Safe from the audio task: no allocation, no blocking, no
 * console. `tag` and `msg` must both have static storage duration — string
 * literals, in practice, which is every caller.
 *
 * Returns false if the ring was full and the warning was dropped. Callers
 * latch their own "already said this" flag regardless, because the point is
 * to say it once, not to guarantee it was heard.
 *
 * The definition carries SYNTH_RENDER_IRAM; this declaration deliberately
 * does not, which is the convention every other header here follows. Each
 * expansion of IRAM_ATTR names its own section, so repeating it on the
 * declaration is a -Werror=attributes conflict rather than a redundancy. */
bool render_warn(const char* tag, const char* msg);

/* Log and clear whatever is queued. Control tasks only — this is the half
 * that touches the console. Call it somewhere that already runs periodically;
 * main.cpp's heartbeat does. Costs one relaxed load when nothing is queued,
 * which is the case on every healthy boot. */
void render_warn_drain(void);

} // namespace osynth::dsp
