/*
 * osynth host port — the test harness.
 *
 * Brings the engine up with osynth_host_start() and exercises it. It owns no
 * init sequence and no render chain of its own: those live in osynth_host.cpp,
 * which is what the app uses too, and a second copy here would be a second
 * thing to keep in step with main.cpp.
 *
 * Three modes:
 *
 *   (default)          play a figure and a chord through the audio device,
 *                      reporting the peak each note produced and the render
 *                      statistics at the end. `<n>` picks an engine.
 *   --storage-write    write a setting and a preset, then exit
 *   --storage-check    verify what a previous --storage-write left
 *   --proto            drive SynthCtl v1 against the engine and check the
 *                      answers (host_prototest.cpp)
 *   --input            route the audio input and meter it for five seconds
 *
 * The storage pair is two processes on purpose: the thing being tested is that
 * state survives a restart, which a single run cannot demonstrate.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "esp_log.h"

#include "audio_io.h"
#include "audio_sink.h"
#include "engines.h"
#include "host_paths.h"
#include "host_prototest.h"
#include "osynth_host.h"
#include "persist.h"
#include "presets.h"
#include "synth_config.h"
#include "synth_params.h"
#include "synth_voice.h"

using osynth::ParamOrigin;
using osynth::ParamStore;

namespace {

const char* const kEngineNames[] = {"subtractive", "additive", "fm",
                                    "wavetable",   "modular",  "granular",
                                    "sampler"};
static_assert(sizeof(kEngineNames) / sizeof(kEngineNames[0]) ==
                  SYNTH_ENGINE_COUNT,
              "engine names must track synth_engine_type_t");

/* Peak of the master bus since the previous call. out_peak is read-and-reset
 * inside audio_io_get_stats(), so each call reports exactly the window since
 * the last one -- which makes it a per-note "did this actually make sound?". */
float peak_since_last() {
    audio_io_stats_t st{};
    audio_io_get_stats(&st);
    return st.out_peak;
}

void play(int note, int velocity, int hold_ms, int rest_ms) {
    voice_manager_note_on((uint8_t)note, (uint8_t)velocity);
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    voice_manager_note_off((uint8_t)note);
    /* The rest is measured with the note, not after it. The FX bus keeps
     * ringing once a note is released -- reverb tail, delay repeats -- and
     * without a gap each note's peak window catches the previous note's tail
     * as well as its own, which made the first note of a phrase read 2.5x low
     * against every note after it for no reason but the order played. */
    std::this_thread::sleep_for(std::chrono::milliseconds(rest_ms));
    std::printf("  note %3d  peak %.3f\n", note, (double)peak_since_last());
    std::fflush(stdout);
}

void report() {
    audio_io_stats_t st{};
    audio_io_get_stats(&st);
    std::printf(
        "\n  blocks %u | underruns %u | sink starves %u | sink errors %u\n"
        "  dsp load %.1f%% (peak %.1f%%) | out peak %.2f | soft clips %u\n",
        (unsigned)st.blocks_rendered, (unsigned)st.underruns,
        (unsigned)audio_sink_host_starves(), (unsigned)st.sink_errors,
        (double)st.dsp_load_pct, (double)st.dsp_load_peak_pct,
        (double)st.out_peak, (unsigned)st.soft_clips);
    std::fflush(stdout);
}

/* Does storage actually round-trip?
 *
 * Worth testing on its own, because "it compiled and booted" says nothing
 * about it: every storage path here is a shim over a different mechanism than
 * the firmware's -- a directory for a LittleFS partition, a file for NVS, a
 * directory for an SD card -- and the failure they would share is writing
 * happily and reading back nothing.
 *
 * Deliberately end-to-end, through presets_request_save() and persist's own
 * save path rather than through the shims directly: the shims are the part
 * already known to work in isolation, and what is being tested is that the
 * components on top of them still behave. It has already earned its keep --
 * it is what caught paths being silently truncated into the wrong file. */
int storage_test(bool check) {
    ParamStore& ps = ParamStore::instance();
    const float kWant = 0.42f;
    const int kSlot = PRESETS_FACTORY_SLOTS; /* the first user slot */

    /* A *patch* parameter, for the working state (state.osw). Deliberately
     * not another global: master.volume belongs to persist/NVS, and
     * persist_owns() is the fence that stops the two stores from both
     * claiming a parameter -- so testing only globals would never touch the
     * working state at all.
     *
     * osc2.semi is engine-owned (0x02xx) and the subtractive engine is what
     * boots, so it is present, and it is patch data by every definition. */
    const uint16_t kPid = 0x0204; /* osc2.semi */
    const float kSemi = 7.0f;

    if (!check) {
        std::printf("\n-- writing --\n");
        ps.set(osynth::PID_MASTER_VOLUME, kWant, ParamOrigin::Ble);
        ps.set(kPid, kSemi, ParamOrigin::Ble);
        std::printf("  osc2.semi = %.1f\n", (double)ps.get(kPid));
        std::printf("  master.volume = %.2f\n",
                    (double)ps.get(osynth::PID_MASTER_VOLUME));

        /* persist coalesces, settles for three seconds and then waits for the
         * output to fall quiet -- the whole point of that design is that it
         * does NOT write when asked. This is the documented way to demand one
         * anyway. */
        std::printf("  persist_save_now: %s\n",
                    esp_err_to_name(persist_save_now()));
        std::printf("  presets_request_save(0, %d): %s\n", kSlot,
                    esp_err_to_name(presets_request_save(0, kSlot, "hosttest")));
        /* The preset task does the work; give it a moment to land. */
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        /* And the working state, which is what osynth_host_stop() writes on a
         * real shutdown. Called explicitly so the test does not depend on how
         * this process happens to exit. */
        std::printf("  presets_state_save_now: %s\n",
                    esp_err_to_name(presets_state_save_now()));

        std::printf("\n  now run:  osynth_host_demo --storage-check\n");
        return 0;
    }

    std::printf("\n-- checking --\n");
    int bad = 0;

    const float got = ps.get(osynth::PID_MASTER_VOLUME);
    const bool vol_ok = got > kWant - 0.01f && got < kWant + 0.01f;
    std::printf("  master.volume: %.2f  %s\n", (double)got,
                vol_ok ? "OK" : "FAIL (expected 0.42)");
    if (!vol_ok) ++bad;

    char name[PRESETS_NAME_MAX] = {};
    bool factory = false;
    const bool preset_ok = presets_slot_info(0, kSlot, name, &factory);
    std::printf("  preset slot %d: %s\n", kSlot,
                preset_ok ? name : "MISSING");
    if (!preset_ok) ++bad;

    /* The working state. osynth_host_start() restored it before this ran, so
     * the value is already in the store and nothing here has to load it --
     * which is the whole point: the app gets its patch back by starting. */
    const float semi = ps.get(kPid);
    const bool semi_ok = semi > kSemi - 0.01f && semi < kSemi + 0.01f;
    std::printf("  osc2.semi restored: %.1f  %s\n", (double)semi,
                semi_ok ? "OK" : "FAIL (expected 7.0)");
    if (!semi_ok) ++bad;

    std::printf("\n  %s\n", bad == 0 ? "storage round-trips" : "STORAGE FAULT");
    return bad == 0 ? 0 : 1;
}

#if SYNTH_ENABLE_AUDIO_IN
/* Is the capture side actually delivering samples?
 *
 * "The device opened" is not the same claim: a capture stream can run and hand
 * back silence forever if the wrong device is default, if the OS denied
 * microphone access, or if the callback's input pointer is null on this
 * backend. The meters answer it directly -- in_peak_* are taken at the capture,
 * before in.gain, precisely so they read the converter rather than the mix.
 *
 * Routes the input to `mon` for the duration, since a route of `off` means
 * nothing reads the block at all. Restores it afterwards: it is a persisted
 * setting and a test has no business changing what the player left. */
int input_test() {
    ParamStore& ps = ParamStore::instance();
    const float saved = ps.get(osynth::PID_LINE_IN_ROUTE);

    std::printf("\n-- audio input --\n");
    std::printf("  make some noise near the default capture device...\n\n");
    /* 1 = mon: heard, and never printed into a looper take. */
    ps.set(osynth::PID_LINE_IN_ROUTE, 1.0f, ParamOrigin::Ble);

    float best = 0.0f;
    uint32_t starves = 0;
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        audio_io_stats_t st{};
        audio_io_get_stats(&st);
        const float pk = st.in_peak_l[0] > st.in_peak_r[0] ? st.in_peak_l[0]
                                                           : st.in_peak_r[0];
        if (pk > best) best = pk;
        starves = st.in_starves[0];

        char bar[41];
        const int n = (int)(pk * 40.0f + 0.5f);
        for (int b = 0; b < 40; ++b) bar[b] = (b < n) ? '#' : '.';
        bar[40] = '\0';
        std::printf("  L %.3f  R %.3f  [%s]\n", (double)st.in_peak_l[0],
                    (double)st.in_peak_r[0], bar);
        std::fflush(stdout);
    }

    ps.set(osynth::PID_LINE_IN_ROUTE, saved, ParamOrigin::Ble);

    std::printf("\n  peak %.3f | capture starves %u | dropped %u\n",
                (double)best, (unsigned)starves,
                (unsigned)audio_sink_host_capture_dropped());
    if (best <= 0.0f) {
        std::printf("  NO SIGNAL — check the default capture device and "
                    "microphone permission\n");
        return 1;
    }
    std::printf("  capture delivers samples\n");
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    int engine = SYNTH_ENGINE_SUBTRACTIVE;
    int storage_mode = 0; /* 1 = write, 2 = check */
    if (argc > 1) {
        if (std::strcmp(argv[1], "--storage-write") == 0) {
            storage_mode = 1;
        } else if (std::strcmp(argv[1], "--storage-check") == 0) {
            storage_mode = 2;
        } else if (std::strcmp(argv[1], "--proto") == 0) {
            storage_mode = 3;
        } else if (std::strcmp(argv[1], "--input") == 0) {
            storage_mode = 4;
        } else {
            engine = std::atoi(argv[1]);
            if (engine < 0 || engine >= SYNTH_ENGINE_COUNT) {
                engine = SYNTH_ENGINE_SUBTRACTIVE;
            }
        }
    }

    osynth_host_config_t cfg;
    osynth_host_config_default(&cfg);
    const esp_err_t err = osynth_host_start(&cfg);
    if (err != ESP_OK) {
        std::fprintf(stderr, "engine failed to start: %s\n",
                     esp_err_to_name(err));
        return 1;
    }
    std::printf("\ndata: %s\nsink: %s\n", osynth_host_data_dir(),
                audio_io_sink_name());

    if (storage_mode == 3) {
        const int rc = osynth_host_prototest();
        osynth_host_stop();
        return rc;
    }
    if (storage_mode == 4) {
#if SYNTH_ENABLE_AUDIO_IN
        const int rc = input_test();
#else
        std::printf("\n  this build has no audio input\n");
        const int rc = 1;
#endif
        osynth_host_stop();
        return rc;
    }
    if (storage_mode != 0) {
        const int rc = storage_test(storage_mode == 2);
        osynth_host_stop();
        return rc;
    }

    /* After the engine is up: engines_init() has already bound whatever
     * engine.type the settings restored, so this goes through the same
     * hot-swap protocol the app's engine picker uses rather than around it. */
    ParamStore& ps = ParamStore::instance();
    if (engine != (int)engines_active_type()) {
        ps.set(osynth::PID_ENGINE_TYPE, (float)engine, ParamOrigin::Ble);
        /* The switch runs on its own task and fades the voice bus over ~10 ms;
         * wait for that plus scheduling slack before playing into it. */
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    std::printf("engine: %s\n\n", kEngineNames[(int)engines_active_type()]);

    /* Clear the meter so the first note's window starts here rather than
     * carrying whatever the pre-roll produced. */
    (void)peak_since_last();

    /* A rising figure, then a held chord: the first says pitch mapping and the
     * envelopes work, the second says polyphony and the master bus survive
     * several voices at once -- which is where a mis-shimmed spinlock or a
     * mis-sized pool would show. */
    const int kMelody[] = {60, 64, 67, 72, 67, 64};
    for (int n : kMelody) play(n, 100, 260, 500);

    std::printf("  chord 60/64/67\n");
    std::fflush(stdout);
    voice_manager_note_on(60, 100);
    voice_manager_note_on(64, 100);
    voice_manager_note_on(67, 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    voice_manager_note_off(60);
    voice_manager_note_off(64);
    voice_manager_note_off(67);

    /* Let the FX tails run out before tearing the device down, so the last
     * thing heard is a decay and not a cut. */
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    report();
    osynth_host_stop();
    return 0;
}
