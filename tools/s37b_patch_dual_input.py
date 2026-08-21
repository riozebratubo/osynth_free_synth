#!/usr/bin/env python3
"""S37b: replace audio_io.cpp's single-device capture with the per-device one.

`in.source` gained a `both` value, so the capture reads every compiled device
into its own slot instead of one selected device into a shared buffer, and each
device carries its own smoothed gain (which also replaced the mute-swap-unmute
fence the exclusive selector needed).

Idempotent: refuses to run twice, because the anchor it splices between only
exists in the pre-S37b file.

Usage:  python tools/s37b_patch_dual_input.py
"""
import io
import sys

PATH = "components/audio_io/audio_io.cpp"
START = "/* Capture one block from the selected input device and advance the three"
END = "    portEXIT_CRITICAL(&s_stats_mux);\n}\n#endif /* SYNTH_ENABLE_AUDIO_IN */"

NEW = '''/* Read one device into its slot, meter it, and zero-fill a short read.
 *
 * `invert_r` un-inverts the differential pair's second leg, and is true only
 * for the line input on an ES8388 wired that way (SYNTH_LINE_IN_INVERT_R in
 * synth_config.h). It must never be applied to the microphone: that block
 * already arrives as one coherent signal duplicated across both channels, and
 * inverting one leg of it would fold to exact silence in every mono take —
 * the same trap this un-inversion exists to close, re-opened from the other
 * side. Doing it here, before anything reads the block, is what keeps every
 * consumer (the mix stages, the graph's LineIn node, the meters) seeing one
 * coherent signal instead of two opposed ones.
 *
 * The read's return code carries nothing the frame count does not: every
 * failure mode, timeout or otherwise, arrives as a short read. A device that
 * never came up takes the same path — no frames, so the block zero-fills and
 * its starve counter climbs, which is exactly what "the RX side is not
 * clocking" already means. */
void SYNTH_RENDER_IRAM capture_one(int slot, size_t got, bool invert_r) {
    int16_t* buf = s_cap[slot];
    const bool starved = (got < SYNTH_BLOCK_SIZE);
    if (starved) {
        memset(buf + got * 2, 0,
               (SYNTH_BLOCK_SIZE - got) * 2 * sizeof(int16_t));
    }

#if SYNTH_LINE_IN_INVERT_R
    /* INT16_MIN has no positive counterpart, so it clamps to INT16_MAX; one
     * LSB at full scale, on a sample already at the converter's rail. */
    if (invert_r) {
        for (size_t i = 0; i < got * 2; i += 2) {
            const int16_t r = buf[i + 1];
            buf[i + 1] = (r == INT16_MIN) ? INT16_MAX : (int16_t)(-r);
        }
    }
#else
    (void)invert_r;
#endif

    /* Integer max-abs over what actually arrived: one compare per sample and
     * no float work in the metering path. */
    int32_t pk_l = 0, pk_r = 0, pk_m = 0;
    for (size_t i = 0; i < got * 2; i += 2) {
        const int32_t al = (buf[i] < 0) ? -(int32_t)buf[i] : (int32_t)buf[i];
        const int32_t ar =
            (buf[i + 1] < 0) ? -(int32_t)buf[i + 1] : (int32_t)buf[i + 1];
        if (al > pk_l) pk_l = al;
        if (ar > pk_r) pk_r = ar;
        /* The same fold the looper's mono take applies, so the meter answers
         * the question a per-channel peak cannot: is there anything left of
         * this input once L and R are summed? */
        const int32_t m = ((int32_t)buf[i] + (int32_t)buf[i + 1]) / 2;
        const int32_t am = (m < 0) ? -m : m;
        if (am > pk_m) pk_m = am;
    }
    const float peak_l = (float)pk_l * kInScale;
    const float peak_r = (float)pk_r * kInScale;
    const float peak_m = (float)pk_m * kInScale;

    portENTER_CRITICAL(&s_stats_mux);
    if (peak_l > s_stats.in_peak_l[slot]) s_stats.in_peak_l[slot] = peak_l;
    if (peak_r > s_stats.in_peak_r[slot]) s_stats.in_peak_r[slot] = peak_r;
    if (peak_m > s_stats.in_peak_mono[slot]) {
        s_stats.in_peak_mono[slot] = peak_m;
    }
    if (starved) s_stats.in_starves[slot]++;
    portEXIT_CRITICAL(&s_stats_mux);
}

/* Capture every compiled input device and advance the route and device gains.
 * Called from the audio task after the cycle counter starts (so the cost lands
 * honestly in dsp_load_pct) and before the render chain (so the block just
 * captured is the one the chain mixes).
 *
 * Every device is read every block, including one `in.source` is not currently
 * selecting, and that is deliberate rather than lazy. It costs about 0.7% of
 * the block budget per device, and it buys two things: a device that has just
 * been selected hands over *current* audio instead of whatever its DMA ring
 * accumulated while nobody was reading it, and its meters stay live so a
 * microphone can be seen working before anything is switched to it. The
 * alternative saves 0.7% and makes every switch open with a burst of stale
 * sound.
 *
 * Never blocks, and never needs to drain. Both sources read with a zero
 * timeout, and a short read is zero-filled and counted. Latency cannot
 * accumulate either: the driver's RX ISR drops the oldest DMA descriptor once
 * the queue fills, and the read skips forward when the queue is nearly full,
 * so the backlog self-clamps at one or two buffers. Sharing BCLK with the DAC
 * does the rest — capture and playback run off one clock, so there is nothing
 * to resample and nothing to drift. The mic shares it too under
 * OSYNTH_MIC_SHARE_CLOCKS, which is why that is the default; a mic mastering
 * its own pins sits at a fixed phase offset instead, which the DMA ring
 * absorbs the same way it absorbs everything else at the same nominal rate. */
void SYNTH_RENDER_IRAM audio_in_capture(void) {
    if (!s_in_ok) return; /* gains stay at 0; the stages early-out */

    size_t got = 0;
#if SYNTH_ENABLE_LINE_IN
    got = 0;
    if (s_line_ok) {
        (void)audio_source_i2s_read(s_cap[kSlotLine], SYNTH_BLOCK_SIZE, &got);
    }
    capture_one(kSlotLine, got, /*invert_r=*/true);
#endif
#if SYNTH_ENABLE_MIC_IN
    got = 0;
    if (s_mic_ok) {
        (void)audio_source_mic_read(s_cap[kSlotMic], SYNTH_BLOCK_SIZE, &got);
    }
    capture_one(kSlotMic, got, /*invert_r=*/false);
#endif
    (void)got;

    const int route =
        (int)(s_in_route->load(std::memory_order_relaxed) + 0.5f);
    const float g = s_in_gain->load(std::memory_order_relaxed);
    for (int i = 0; i < kInPositions; ++i) {
        s_in_g[i] = osynth::dsp::smooth_lin(s_in_sm[i],
                                            (route == i + 1) ? g : 0.0f);
    }

#if SYNTH_ENABLE_IN_SOURCE_SEL
    /* Device gains. `both` is the only value that leaves two of them non-zero,
     * and the mic carries its own trim because the two devices do not arrive
     * anywhere near each other in level: a line source sits where the ADC was
     * set up to take it, a MEMS mic at conversational distance sits far below
     * full scale. One shared `in.gain` across both would be a control that is
     * wrong for one of them whichever way it is set — which is the whole
     * reason `in.micgain` exists, and why it is registered only where there
     * are two devices for it to sit between. */
    const int sel = (int)(s_in_source->load(std::memory_order_relaxed) + 0.5f);
    const float micg = s_in_micgain->load(std::memory_order_relaxed);
    const bool line_on = (sel == kSelLine || sel == kSelBoth);
    const bool mic_on = (sel == kSelMic || sel == kSelBoth);
    s_dev_g[kSlotLine] =
        osynth::dsp::smooth_lin(s_dev_sm[kSlotLine], line_on ? 1.0f : 0.0f);
    s_dev_g[kSlotMic] =
        osynth::dsp::smooth_lin(s_dev_sm[kSlotMic], mic_on ? micg : 0.0f);
#else
    /* One device compiled in: it is the source, always, and its gain is a
     * constant the compiler folds into the mix below. */
    s_dev_g[0] = 1.0f;
#endif

    /* Published for the heartbeat, from here rather than by reading the
     * parameter again on the other side: what matters is the route the audio
     * task actually acted on and the gains it actually mixed with, which is
     * the pair a re-read cannot vouch for. Same mux as the peaks above — a
     * second short critical section rather than one widened over the capture,
     * so the metering loop stays outside it. */
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.in_route = (uint8_t)((route < 0) ? 0 : (route > 3) ? 0 : route);
    /* The device gains the audio task actually mixed with, which during a
     * crossfade are both non-zero and are the only thing that says so. */
    for (int d = 0; d < kDevCount; ++d) s_stats.in_dev_g[d] = s_dev_g[d];
    s_stats.in_g[kInMon] = s_in_g[kInMon];
    s_stats.in_g[kInFx] = s_in_g[kInFx];
    s_stats.in_g[kInDry] = s_in_g[kInDry];
'''


def main():
    src = io.open(PATH, encoding="utf-8").read()
    if START not in src:
        print("anchor not found — already patched, or the file moved on")
        return 1
    i = src.index(START)
    j = src.index(END)
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(src[:i] + NEW + src[j:])
    print("patched " + PATH)
    return 0


if __name__ == "__main__":
    sys.exit(main())
