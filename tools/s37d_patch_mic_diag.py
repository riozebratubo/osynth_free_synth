#!/usr/bin/env python3
"""S37d - ESP32-P4 on-board microphone (ES8311): clock source + bring-up telemetry.

Applied once; kept per the project's "keep the intermediary artifacts" rule so
the exact edit stays re-readable next to the reasoning that produced it.

Four changes, in the order they matter:

 1. components/audio_io/source_mic.cpp - put a mic port that *masters its own
    pins* on APLL first. It was on XTAL, which reaches 12.288 MHz only through
    a fractional divider (40/12.288 = 3 + 49/192), so the MCLK the ES8311's
    whole clock manager hangs off dithers between 75 ns and 100 ns periods.
    The board's other codec has been on APLL since the P4 bring-up for exactly
    this reason.
 2. source_mic.cpp - raw-slot telemetry. The heartbeat's %.2f peak cannot tell
    "the pin is dead" from "the signal is 60 dB down", and neither can the
    24-to-16 truncation in front of it. An OR of the raw words' magnitude bits
    can, and it is the one measurement this port has never had.
 3. source_mic.cpp / codec_es8311.cpp - a GPIO matrix dump for the four mic
    pads, and the codec's chip-ID registers, so "the register file matches the
    vendor" rests on reads that are demonstrably real.
 4. sdkconfig.defaults.esp32p4 - mic digital gain 0 -> 3 bits (+18 dB), so a
    quiet-but-present microphone reaches the meter's resolution at all.
"""
import io
import sys


def edit(path, subs):
    with io.open(path, encoding='utf-8', newline='') as f:
        src = f.read()
    for old, new in subs:
        # `new` often contains `old` (these are insertions around a kept
        # anchor), so "is the anchor still there?" is not a usable test for
        # whether this edit has run. Ask about the result instead.
        if new in src:
            print('  (already applied) %r' % old[:50])
            continue
        n = src.count(old)
        if n != 1:
            sys.exit('%s: anchor count %d: %r' % (path, n, old[:70]))
        src = src.replace(old, new)
    with io.open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(src)
    print('patched %s (%d edits)' % (path, len(subs)))


DASH = u'—'  # em dash, matching the surrounding prose

# ---------------------------------------------------------------- source_mic
MIC = 'components/audio_io/source_mic.cpp'
mic_subs = []

mic_subs.append((
    u'''#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"     /* probe_din(): pull the pad before I2S takes it */
''',
    u'''#include <atomic>
#include <stdint.h>
#include <stdio.h> /* gpio_dump_io_configuration() takes a FILE* */
#include <string.h>

#include "driver/gpio.h"     /* probe_din(): pull the pad before I2S takes it */
'''))

mic_subs.append((
    u'''#define OSYNTH_MIC_LOOPBACK_TEST 0
''',
    u'''#define OSYNTH_MIC_LOOPBACK_TEST 0

/* ---- pad dump --------------------------------------------------------------
 *
 * Print what the GPIO matrix actually did with this port's four pads, once, as
 * soon as the port is up.
 *
 * It answers a question every other instrument on this port assumes the answer
 * to: that the driver claimed the pins it was given, as outputs where it
 * should and with the input enabled on DIN. i2s_channel_init_std_mode()
 * returning ESP_OK does not say that {D} a pad another peripheral already holds
 * is a warning in the driver's log and a working channel that reaches nothing,
 * which is a fault this board has produced before (PINMAP.md, GPIO45/46/47).
 *
 * Cheap enough to leave on while the on-board microphone is unresolved: four
 * pads, four lines, once per boot. Turn it off once this port is trusted. */
#define OSYNTH_MIC_DUMP_GPIO 1
'''.replace('{D}', DASH)))

mic_subs.append((
    u''' * **Never APLL.** The output port asks for APLL on targets that have one, and
 * asking is not passive: i2s_get_source_clk_freq() routes an APLL request
 * through i2s_set_get_apll_freq(), which *sets* the PLL to suit the caller.
 * There is one APLL on the chip. A second channel asking it for a different
 * frequency does not fail and does not fall back {D} it retunes the clock the
 * DAC is running on, and the symptom is a microphone that works and an output
 * that has quietly gone out of tune.
'''.replace('{D}', DASH),
    u''' * **APLL where this port masters its own pins, and only there** (S37d). The
 * rule here used to be "never APLL", on the grounds that a second channel
 * asking for a different frequency would retune the clock the DAC runs on.
 * Half of that is wrong and the other half does not apply to a master:
 *
 *   - It cannot retune an occupied APLL. i2s_set_get_apll_freq() goes through
 *     esp_clk_tree_src_set_freq_hz(), which returns ESP_ERR_INVALID_STATE when
 *     the PLL is already claimed, logs "APLL is occupied already ... trying to
 *     work at N Hz", and hands back the frequency it is *actually* running at
 *     for the divider arithmetic. The DAC's clock is not moved by asking.
 *   - There is nothing different to ask for. A master's clk_cfg names
 *     mclk = 256 x fs, the identical figure sink_i2s.cpp asks for on the
 *     output port {D} so both requests compute the same APLL frequency, and the
 *     second one is a no-op by construction.
 *
 * The reason to want it is the reason the output port is on APLL at all: on a
 * 40 MHz XTAL, 12.288 MHz needs a fractional divider (3 + 49/192), so the MCLK
 * this port generates dithers between 75 ns and 100 ns periods {D} 30% of a
 * period, peak to peak. On the output that produced audible distortion from an
 * ES8388 and cost a bring-up (see sink_i2s.cpp). On *this* port the same pin is
 * the only clock an ES8311 has: its clock manager divides MCLK for the ADC's
 * modulator and decimator and relates the result to the incoming LRCK, and
 * every reference design feeds it a clean 256 x fs.
 *
 * A second consequence, free: with MCLK, BCLK and WS all off the APLL the sink
 * uses, a mic mastering its own pins is sample-locked to the output instead of
 * sitting in a second clock domain that drifts. That was listed as the price of
 * not sharing clocks, and it is no longer one.
 *
 * **Still never APLL for a slave.** There the declared sample rate is divided
 * to make the internal clock legal (see mic_init_std), so the request genuinely
 * would differ from the DAC's {D} and it buys nothing, because a slave's timing
 * comes from the master's pins and this clock never leaves the peripheral.
'''.replace('{D}', DASH)))

mic_subs.append((
    u'''const ClockAttempt kClockAttempts[] = {
#if OSYNTH_MIC_CLK_HAS_PLL160
''',
    u'''const ClockAttempt kClockAttempts[] = {
#if !SYNTH_MIC_SHARE_CLOCKS && SOC_I2S_SUPPORTS_APLL
    {I2S_CLK_SRC_APLL, 1, "apll"},
#endif
#if OSYNTH_MIC_CLK_HAS_PLL160
'''))

mic_subs.append((
    u'''mic_word_t s_raw[SYNTH_BLOCK_SIZE * 2];
''',
    u'''mic_word_t s_raw[SYNTH_BLOCK_SIZE * 2];

/* Every bit that has appeared on either slot since the last read (S37d).
 *
 * This exists because nothing else on this port can distinguish a data pin
 * nothing drives from one carrying a signal too quiet to survive the pipeline
 * in front of the meter. Two stages crush it: narrow() keeps the top 16 bits of
 * a 24-bit word, and the heartbeat prints the result as %.2f {D} so everything
 * below about -46 dBFS reads as exactly `mic 0.00/0.00`, which is also what a
 * disconnected pin reads. Five rounds of ES8311 register work were spent inside
 * that ambiguity.
 *
 * An OR of the *magnitudes* resolves it and carries the level with it: the
 * highest set bit is the loudest sample the window saw, and 0x00000000 across a
 * whole window means the pin never left zero {D} which no register change can
 * fix, and which is the point at which the fault is in copper rather than code.
 *
 * Relaxed atomics, folded once per block rather than once per sample: this runs
 * in the render path, and the reader is a heartbeat that cares about the value
 * and not about which block it landed in. */
std::atomic<uint32_t> s_raw_or[2];
'''.replace('{D}', DASH)))

mic_subs.append((
    u'''/* One slot to one int16.
''',
    u'''/* |w| as a bit mask, without the sign extension that would make every negative
 * sample read 0xFFFFFFFF and throw the magnitude away. */
inline uint32_t SYNTH_RENDER_IRAM mag_bits(mic_word_t w) {
    const int32_t v = (int32_t)w;
    return (uint32_t)(v ^ (v >> 31));
}

/* One slot to one int16.
'''))

mic_subs.append((
    u'''    for (size_t i = 0; i < got; ++i) {
#if defined(CONFIG_OSYNTH_MIC_STEREO)
        interleaved[i * 2] = narrow(s_raw[i * 2]);
        interleaved[i * 2 + 1] = narrow(s_raw[i * 2 + 1]);
#else''',
    u'''    uint32_t or_l = 0, or_r = 0;
    for (size_t i = 0; i < got; ++i) {
        /* Both slots, whichever one is being used: which half of the frame a
         * mono converter lands in is a property of its own output routing, and
         * seeing the other one is how that gets settled rather than guessed. */
        or_l |= mag_bits(s_raw[i * 2]);
        or_r |= mag_bits(s_raw[i * 2 + 1]);
#if defined(CONFIG_OSYNTH_MIC_STEREO)
        interleaved[i * 2] = narrow(s_raw[i * 2]);
        interleaved[i * 2 + 1] = narrow(s_raw[i * 2 + 1]);
#else'''))

mic_subs.append((
    u'''    *frames_read = got;
    return err;
}
''',
    u'''    s_raw_or[0].fetch_or(or_l, std::memory_order_relaxed);
    s_raw_or[1].fetch_or(or_r, std::memory_order_relaxed);

    *frames_read = got;
    return err;
}

void audio_source_mic_raw_take(uint32_t* or_l, uint32_t* or_r) {
    *or_l = s_raw_or[0].exchange(0, std::memory_order_relaxed);
    *or_r = s_raw_or[1].exchange(0, std::memory_order_relaxed);
}
'''))

mic_subs.append((
    u'''} // namespace

/* Is anything driving the data pin at all?
''',
    u'''/* 1 << pin, and 0 for I2S_GPIO_UNUSED (-1). A function rather than the obvious
 * expression because shifting by a negative count is undefined even in the arm
 * of a conditional the compiler can fold away. */
inline uint64_t pin_mask(gpio_num_t pin) {
    return ((int)pin >= 0) ? (1ULL << (int)pin) : 0ULL;
}

} // namespace

/* Is anything driving the data pin at all?
'''))

mic_subs.append((
    u'''    ESP_LOGI(TAG, "mic: %s, digital gain %d bits (+%d dB)", kChannelName,
             kShift, kShift * 6);
''',
    u'''    ESP_LOGI(TAG, "mic: %s, digital gain %d bits (+%d dB)", kChannelName,
             kShift, kShift * 6);
#if OSYNTH_MIC_DUMP_GPIO
    /* Read each row for one thing: the clock pins claimed as *outputs* with an
     * I2S signal on them, and DIN claimed as an *input*. A pad that is neither
     * is one the matrix never took, which is the difference between a port that
     * is misconfigured and a port that is not connected to anything. */
    (void)gpio_dump_io_configuration(
        stdout, pin_mask(OSYNTH_MIC_DIN) | pin_mask(OSYNTH_MIC_BCLK) |
                    pin_mask(OSYNTH_MIC_WS) | pin_mask(OSYNTH_MIC_MCLK));
#endif
'''))

edit(MIC, mic_subs)

# ---------------------------------------------------------------- audio_sink
edit('components/audio_io/audio_sink.h', [(
    u'''esp_err_t audio_source_mic_read(int16_t* interleaved, size_t frames,
                                size_t* frames_read);
#endif
''',
    u'''esp_err_t audio_source_mic_read(int16_t* interleaved, size_t frames,
                                size_t* frames_read);

/* Every bit seen on each raw slot since the last call, and clears the
 * accumulator (S37d). The control-task side of the bring-up telemetry described
 * in source_mic.cpp {D} see there for why a peak meter cannot answer the same
 * question. Reads 0 on a mic that never came up, which is also what a mic with
 * a dead data pin reads; the boot log separates those two. */
void audio_source_mic_raw_take(uint32_t* or_l, uint32_t* or_r);
#endif
'''.replace('{D}', DASH))])

# ------------------------------------------------------------------ audio_io
edit('components/audio_io/include/audio_io.h', [(
    u'''    float in_dev_g[2];
} audio_io_stats_t;
''',
    u'''    float in_dev_g[2];
    /* Microphone only (S37d), and raw: every bit that appeared on each I2S slot
     * since the previous audio_io_get_stats() call, before narrowing and before
     * any gain. 0 on a build with no microphone.
     *
     * This is the meter for the question the peaks above cannot answer. They
     * are taken after the 24-to-16 truncation and printed as %.2f, so a
     * microphone 46 dB down and a data pin nobody drives both read 0.00. Here
     * the first is a small number and the second is exactly zero. */
    uint32_t mic_raw_or[2];
} audio_io_stats_t;
''')])

edit('components/audio_io/audio_io.cpp', [(
    u'''        s_stats.in_peak_mono[d] = 0.0f;
    }
    portEXIT_CRITICAL(&s_stats_mux);
}
''',
    u'''        s_stats.in_peak_mono[d] = 0.0f;
    }
    portEXIT_CRITICAL(&s_stats_mux);

    /* Outside the critical section on purpose: it is its own read-and-reset
     * pair of atomics in source_mic.cpp, and it does not have to be consistent
     * with the peaks above {D} it answers a question about the pin, not about
     * the block. */
    out->mic_raw_or[0] = 0;
    out->mic_raw_or[1] = 0;
#if SYNTH_ENABLE_MIC_IN
    audio_source_mic_raw_take(&out->mic_raw_or[0], &out->mic_raw_or[1]);
#endif
}
'''.replace('{D}', DASH))])

# ---------------------------------------------------------------------- main
edit('main/main.cpp', [(
    u'''    char in_seg[176]; /* route + three gains (S31), source tag (S37). The''',
    u'''    char in_seg[224]; /* route + three gains (S31), source tag (S37), raw
                      * slot bits (S37d). The'''), (
    u'''        static const char* kRouteSeg[] = {"off", "mon", "fx", "dry"};''',
    u'''        int in_n = 0;
        static const char* kRouteSeg[] = {"off", "mon", "fx", "dry"};'''), (
    u'''        snprintf(in_seg, sizeof(in_seg),
                 " | in line %.2f/%.2f f%.2f g%.2f mic %.2f/%.2f f%.2f g%.2f "
                 "%s %.2f/%.2f/%.2f, starve %u/%u",''',
    u'''        in_n = snprintf(in_seg, sizeof(in_seg),
                 " | in line %.2f/%.2f f%.2f g%.2f mic %.2f/%.2f f%.2f g%.2f "
                 "%s %.2f/%.2f/%.2f, starve %u/%u",'''), (
    u'''        snprintf(in_seg, sizeof(in_seg),
                 " | in pk %.2f/%.2f fold %.2f %s g %.2f/%.2f/%.2f, starve %u",''',
    u'''        in_n = snprintf(in_seg, sizeof(in_seg),
                 " | in pk %.2f/%.2f fold %.2f %s g %.2f/%.2f/%.2f, starve %u",'''), (
    u'''#else
        in_seg[0] = '\\0';
#endif
''',
    u'''#if SYNTH_ENABLE_MIC_IN
        /* The raw microphone slots, before narrowing and before any gain:
         * every bit either slot carried during the window. `raw 000000/000000`
         * means the data pin never left zero for a whole second {D} a fault in
         * copper or in the converter's own output stage, not in anything this
         * firmware can set. Anything non-zero means the converter is talking
         * and the argument moves to level and routing. See the mic_raw_or note
         * in audio_io.h. */
        if (in_n > 0 && (size_t)in_n < sizeof(in_seg)) {
            snprintf(in_seg + in_n, sizeof(in_seg) - (size_t)in_n,
                     " raw %06x/%06x", (unsigned)st.mic_raw_or[0],
                     (unsigned)st.mic_raw_or[1]);
        }
#else
        (void)in_n; /* the length is only needed to append the raw slots */
#endif
#else
        in_seg[0] = '\\0';
#endif
'''.replace('{D}', DASH))])

# ------------------------------------------------------------- codec_es8311
edit('components/codec/codec_es8311.cpp', [(
    u'''    r = 0x45;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &v45, 1, kTimeoutMs);
    ESP_LOGI(TAG, "regs 44: %02x %02x", v44, v45);
}
''',
    u'''    r = 0x45;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &v45, 1, kTimeoutMs);
    ESP_LOGI(TAG, "regs 44: %02x %02x", v44, v45);

    /* Chip ID and version, which is the register dump's own credential.
     *
     * Everything the dump above is used for {D} "the values match the vendor's
     * driver byte for byte" {D} rests on these being real reads of a real
     * ES8311, and nothing had checked that. 83 11 is the part; anything else,
     * and especially 00 00 or ff ff, means the reads are an artefact and every
     * conclusion drawn from them has to go. The version byte is worth having
     * beside it because the analogue front end differs across revisions. */
    uint8_t id1 = 0, id2 = 0, ver = 0;
    r = 0xFD;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &id1, 1, kTimeoutMs);
    r = 0xFE;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &id2, 1, kTimeoutMs);
    r = 0xFF;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &ver, 1, kTimeoutMs);
    ESP_LOGI(TAG, "chip id %02x%02x ver %02x (expect 8311)", id1, id2, ver);
}
'''.replace('{D}', DASH))])

# ------------------------------------------------------- sdkconfig.defaults
edit('sdkconfig.defaults.esp32p4', [(
    u'''# CONFIG_OSYNTH_MIC_SLOT_LEFT=y
# CONFIG_OSYNTH_MIC_STEREO=y
# CONFIG_OSYNTH_MIC_SHIFT=0
''',
    u'''# CONFIG_OSYNTH_MIC_SLOT_LEFT=y
# CONFIG_OSYNTH_MIC_STEREO=y

# +18 dB at the 24-to-16 truncation, and on this target that is a measurement
# problem before it is a taste one. The heartbeat prints the input peak as
# %.2f, so with no shift at all anything below about -46 dBFS reads 0.00 {D}
# indistinguishable from a dead pin, which is exactly the ambiguity the ES8311
# bring-up kept landing in. Three bits of free gain (taken from precision the
# int16 path discards anyway, and saturating rather than wrapping) put a quiet
# analogue mic and a MEMS part at conversational distance both on the scale.
# `in.micgain` is still the runtime trim; this only decides where the meter
# starts being able to see anything.
CONFIG_OSYNTH_MIC_SHIFT=3
'''.replace('{D}', DASH))])

print('done')
