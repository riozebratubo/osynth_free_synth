#!/usr/bin/env python3
"""S37f - fix the pad probe's timing, and add the codec-masters-the-bus test.

The S37e boot log made two things clear.

**The pad probe ran too early to mean anything about DIN.** It sits at the end
of audio_source_mic_start(), which is inside audio_io_start(); codec_mic_init()
runs after that. So `pad probe: din GPIO11 is STUCK at 0` was measured while the
ES8311 was still in its power-on state and had been told nothing — of course it
was stuck. The clock pads it measured *are* meaningful (bclk 50%, ws 50%,
mclk 49%: all three switch, so this is not the GPIO45/46/47 failure), but DIN
has to be read again once the codec is configured. That call now exists, and the
probe takes a label so the two readings are told apart in the log.

**The `reset` dump was not a reset dump.** It came back byte-for-byte identical
to the `init` dump — REG09/0A at 0x10, REG16 at 0x24, REG17 at 0xBF, REG44 at
0x58 — which are our written values, not an ES8311's power-on defaults. The
codec keeps its rail across a P4 reset, so every warm boot starts from whatever
the previous boot left. Two consequences: the masked writes added in S37e cannot
be observed to do anything until a genuine power cycle, and every "tried X, still
silent" result in this file's history was taken on a chip carrying accumulated
state from the attempt before it. No code fixes that; the dump's own log line
now says so.

Three changes:

 1. probe_clock_pins() becomes audio_source_mic_probe_pads(when), is always
    compiled, and is called again from main.cpp after codec_mic_init().
 2. The sweep picks up one extra pad — GPIO9, the board's ES8311 DSDIN — and
    pulls each pad down while sampling, so an undriven pin reads a solid 0
    instead of wandering. If GPIO9 turns out to toggle, the board's data pins
    are the other way round from the ESPHome transcription.
 3. SYNTH_MIC_CODEC_MASTER_TEST: one switch that makes the ES8311 the I2S
    master (REG00 bit 6) and takes the P4 off BCLK and WS entirely
    (I2S_GPIO_UNUSED, which i2s_gpio_check_and_set skips), leaving it driving
    only MCLK. The pad probe then reads GPIO12 and GPIO10 with nothing on this
    chip driving them: if the codec is receiving MCLK it will generate BCLK and
    LRCK there, and if it is not they stay stuck. That is the last firmware-only
    way to prove the clocks reach the part, and it needs no scope.

Kept per the project's artifacts rule.
"""
import io
import sys


def edit(path, subs):
    with io.open(path, encoding='utf-8', newline='') as f:
        src = f.read()
    for old, new in subs:
        if new in src:
            print('  (already applied) %r' % old[:50])
            continue
        n = src.count(old)
        if n != 1:
            sys.exit('%s: anchor count %d: %r' % (path, n, old[:70]))
        src = src.replace(old, new)
    with io.open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(src)
    print('patched %s' % path)


D = u'—'  # em dash

# --------------------------------------------------------------- synth_config
edit('components/synth_core/include/synth_config.h', [(
    u'''#else
#define SYNTH_MIC_SHARE_CLOCKS 0
#define SYNTH_MIC_SLOT_BITS    16
#endif
''',
    u'''#else
#define SYNTH_MIC_SHARE_CLOCKS 0
#define SYNTH_MIC_SLOT_BITS    16
#endif

/* ---- bring-up: let the microphone codec master the bus (S37f) --------------
 *
 * Off. A one-shot instrument for one unresolved question on the ESP32-P4
 * carrier, and it lives here rather than in either .cpp because it has to move
 * both ends at once: source_mic.cpp stops driving BCLK and WS, and
 * codec_es8311.cpp sets REG00 bit 6. Flipping only one of them puts two drivers
 * on two pins.
 *
 * The question it answers: **do this port's clocks actually reach the codec?**
 * Everything measured so far stops at the pad. The GPIO matrix routes the four
 * signals correctly, and the pads switch (bclk 50%, ws 50%, mclk 49% of a
 * 4096-sample sweep) {D} but a pad that switches at the die says nothing about
 * the far end of the trace, and the ES8311 is configured, identified
 * (`chip id 8311`) and emitting a hard zero on its data pin.
 *
 * With this on, the P4 drives MCLK and nothing else. If the codec is receiving
 * that clock it will divide it into BCLK (MCLK/4) and LRCK (DIG_MCLK/256) and
 * drive them onto GPIO12 and GPIO10 by itself, which the pad probe in
 * source_mic.cpp will report. Stuck pads mean MCLK is not arriving, and the
 * fault is between the pin and the part.
 *
 * Expect `in starve` to climb the whole time it is on: with no BCLK or WS of
 * its own the port never completes a frame, and that is the arrangement, not a
 * symptom. */
#define SYNTH_MIC_CODEC_MASTER_TEST 0
'''.replace('{D}', D))])

# ---------------------------------------------------------------- source_mic
MIC = 'components/audio_io/source_mic.cpp'

edit(MIC, [
    # -- the port stops driving BCLK/WS under the test ----------------------
    (u'''        .gpio_cfg = {
            .mclk = OSYNTH_MIC_MCLK, /* unused unless a codec needs one */
            .bclk = OSYNTH_MIC_BCLK,
            .ws = OSYNTH_MIC_WS,''',
     u'''        .gpio_cfg = {
            .mclk = OSYNTH_MIC_MCLK, /* unused unless a codec needs one */
#if SYNTH_MIC_CODEC_MASTER_TEST
            /* The codec drives these; this chip must not. -1 is skipped by
             * i2s_gpio_check_and_set(), so the pads are left alone entirely
             * rather than claimed and driven. See SYNTH_MIC_CODEC_MASTER_TEST
             * in synth_config.h. */
            .bclk = I2S_GPIO_UNUSED,
            .ws = I2S_GPIO_UNUSED,
#else
            .bclk = OSYNTH_MIC_BCLK,
            .ws = OSYNTH_MIC_WS,
#endif'''),

    # -- the probe itself ----------------------------------------------------
    (u'''#if OSYNTH_MIC_DUMP_GPIO
/* Do this port's clock pads actually toggle? (S37e)''',
     u'''/* Do this port's pads actually toggle? (S37e, retimed in S37f)'''),

    (u'''void probe_clock_pins(void) {
    struct Probe {
        gpio_num_t pin;
        const char* name;
    };
    const Probe kProbes[] = {
        {OSYNTH_MIC_BCLK, "bclk"},
        {OSYNTH_MIC_WS, "ws"},
        {OSYNTH_MIC_MCLK, "mclk"},
        {OSYNTH_MIC_DIN, "din"},
    };
    const int kSamples = 4096;
    for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); ++i) {
        const gpio_num_t pin = kProbes[i].pin;
        if ((int)pin < 0) continue;
        if (gpio_input_enable(pin) != ESP_OK) continue;
        int ones = 0;
        for (int n = 0; n < kSamples; ++n) ones += gpio_get_level(pin);
        if (ones == 0 || ones == kSamples) {
            ESP_LOGW(TAG,
                     "pad probe: %s GPIO%d is STUCK at %d over %d samples {D} "
                     "this pad is not switching, so nothing on the other end of "
                     "it is being clocked",
                     kProbes[i].name, (int)pin, ones ? 1 : 0, kSamples);
        } else {
            ESP_LOGI(TAG, "pad probe: %s GPIO%d toggles (high %d%% of %d)",
                     kProbes[i].name, (int)pin, ones * 100 / kSamples,
                     kSamples);
        }
    }
}
#endif
'''.replace('{D}', D),
     u'''void audio_source_mic_probe_pads(const char* when) {
    struct Probe {
        gpio_num_t pin;
        const char* name;
    };
    const Probe kProbes[] = {
        {OSYNTH_MIC_BCLK, "bclk"},
        {OSYNTH_MIC_WS, "ws"},
        {OSYNTH_MIC_MCLK, "mclk"},
        {OSYNTH_MIC_DIN, "din"},
        {(gpio_num_t)OSYNTH_MIC_PROBE_EXTRA_GPIO, "extra"},
    };
    const int kSamples = 4096;
    for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); ++i) {
        const gpio_num_t pin = kProbes[i].pin;
        if ((int)pin < 0) continue;
        if (gpio_input_enable(pin) != ESP_OK) continue;
        /* Pull down while sampling. A pad this chip is driving push-pull does
         * not notice a 45k pull, and a pad nobody drives stops wandering {D}
         * without which an undriven CMOS input can read as "toggles" and turn
         * this instrument into a source of false positives, which is exactly
         * what it exists to stop. */
        (void)gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        int ones = 0;
        for (int n = 0; n < kSamples; ++n) ones += gpio_get_level(pin);
        (void)gpio_set_pull_mode(pin, GPIO_FLOATING);
        if (ones == 0 || ones == kSamples) {
            ESP_LOGW(TAG,
                     "pad probe (%s): %s GPIO%d is STUCK at %d over %d samples "
                     "{D} this pad is not switching",
                     when, kProbes[i].name, (int)pin, ones ? 1 : 0, kSamples);
        } else {
            ESP_LOGI(TAG, "pad probe (%s): %s GPIO%d toggles (high %d%% of %d)",
                     when, kProbes[i].name, (int)pin,
                     ones * 100 / kSamples, kSamples);
        }
    }
}
'''.replace('{D}', D)),

    # -- the extra pad -------------------------------------------------------
    (u'''#define OSYNTH_MIC_DUMP_GPIO 1
''',
     u'''#define OSYNTH_MIC_DUMP_GPIO 1

/* One more pad for the sweep in audio_source_mic_probe_pads(), or -1.
 *
 * On the ESP32-P4 carrier that is GPIO9, which the board's own configuration
 * calls the ES8311's *DSDIN* {D} an input to the codec, so it should sit still.
 * It is swept because "should" is doing a lot of work there: the pin roles come
 * from an ESPHome block, not a schematic, and if GPIO9 turns out to be the pad
 * that moves then the data lines are simply the other way round and GPIO11 is
 * the codec's input, which would explain a data pin that is held low and never
 * changes no matter what the ADC is told to do. */
#if SYNTH_ENABLE_CODEC_ES8311
#define OSYNTH_MIC_PROBE_EXTRA_GPIO 9
#else
#define OSYNTH_MIC_PROBE_EXTRA_GPIO -1
#endif
'''.replace('{D}', D)),

    # -- call site -----------------------------------------------------------
    (u'''    (void)gpio_dump_io_configuration(
        stdout, pin_mask(OSYNTH_MIC_DIN) | pin_mask(OSYNTH_MIC_BCLK) |
                    pin_mask(OSYNTH_MIC_WS) | pin_mask(OSYNTH_MIC_MCLK));
    probe_clock_pins();
#endif
''',
     u'''    (void)gpio_dump_io_configuration(
        stdout, pin_mask(OSYNTH_MIC_DIN) | pin_mask(OSYNTH_MIC_BCLK) |
                    pin_mask(OSYNTH_MIC_WS) | pin_mask(OSYNTH_MIC_MCLK));
#endif
    /* The baseline: clocks running, codec not yet told anything. main.cpp takes
     * the same reading again after codec_mic_init(), and the pair is the point
     * {D} DIN standing still here means nothing at all, since nothing has asked
     * the converter for a sample yet. */
    audio_source_mic_probe_pads("port up");
'''.replace('{D}', D)),
])

# ---------------------------------------------------------------- audio_sink
edit('components/audio_io/audio_sink.h', [(
    u'''void audio_source_mic_raw_take(uint32_t* or_l, uint32_t* or_r);
#endif
''',
    u'''void audio_source_mic_raw_take(uint32_t* or_l, uint32_t* or_r);

/* Sweeps this port's pads for activity and logs one line each, tagged with
 * `when` (S37f). Boot-time only: it busy-polls each pad a few thousand times.
 * See the definition in source_mic.cpp for what the readings mean. */
void audio_source_mic_probe_pads(const char* when);
#endif
''')])

# ------------------------------------------------------------------ audio_io
edit('components/audio_io/include/audio_io.h', [(
    u'''void audio_io_get_stats(audio_io_stats_t* out);
''',
    u'''void audio_io_get_stats(audio_io_stats_t* out);

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
''')])

edit('components/audio_io/audio_io.cpp', [(
    u'''void audio_io_get_stats(audio_io_stats_t* out) {''',
    u'''void audio_io_mic_probe_pads(void) {
#if SYNTH_ENABLE_MIC_IN
    if (s_mic_ok) audio_source_mic_probe_pads("codec up");
#endif
}

void audio_io_get_stats(audio_io_stats_t* out) {''')])

# ---------------------------------------------------------------------- main
edit('main/main.cpp', [(
    u'''    (void)codec_mic_init();
#endif
    ESP_LOGI(TAG, "audio sink: %s | codec: %s", audio_io_sink_name(),
             codec_name());
''',
    u'''    (void)codec_mic_init();
#endif
    /* The second pad reading, and the one that carries information: the sweep
     * inside audio_io_start() happens before any input codec has been told
     * anything, so a data pin standing still there is the expected result. If
     * it is still standing still here, configuring the converter changed
     * nothing on the wire. */
    audio_io_mic_probe_pads();
    ESP_LOGI(TAG, "audio sink: %s | codec: %s", audio_io_sink_name(),
             codec_name());
''')])

# ------------------------------------------------------------- codec_es8311
edit('components/codec/codec_es8311.cpp', [(
    u'''/* REG_RESET = 0x80: out of reset, slave. Bit 6 is master mode and stays clear —
 * the P4's mic port is the master on this bus (it drives BCLK, WS and MCLK),
 * so the codec must not also try to. */
constexpr uint8_t kResetRun = 0x80;
''',
    u'''/* REG_RESET = 0x80: out of reset, slave. Bit 6 is master mode and normally
 * stays clear {D} the P4's mic port is the master on this bus (it drives BCLK,
 * WS and MCLK), so the codec must not also try to.
 *
 * SYNTH_MIC_CODEC_MASTER_TEST (synth_config.h) inverts that, and the two ends
 * move together: source_mic.cpp stops driving BCLK and WS in the same build.
 * See that switch for what the arrangement is for. */
#if SYNTH_MIC_CODEC_MASTER_TEST
constexpr uint8_t kResetRun = 0xC0;
#else
constexpr uint8_t kResetRun = 0x80;
#endif
'''.replace('{D}', D)), (
    u'''    dump_regs("reset");
''',
    u'''    /* **Warm boots do not reset this chip.** It keeps its rail across a P4
     * reset, so on anything but a cold start this dump is the previous boot's
     * end state, not a power-on default {D} which is exactly what it came back
     * as the first time it ran, byte for byte identical to the `init` dump
     * below. Two things follow, and neither is fixable in code: the masked
     * writes above cannot be seen to preserve anything until the board has been
     * fully unpowered, and every earlier "tried it, still silent" result was
     * taken on a chip carrying whatever the attempt before it left behind. */
    dump_regs("reset");
'''.replace('{D}', D))])

print('done')
