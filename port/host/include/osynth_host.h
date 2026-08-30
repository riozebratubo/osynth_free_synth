/*
 * osynth host port — bringing the whole engine up in a host process.
 *
 * This is the host's app_main(): it registers the global parameters, starts
 * every component in the order main.cpp starts them, and runs the audio task
 * against the miniaudio sink. After it returns ESP_OK there is a complete,
 * sounding synth in the process, reachable through the same ParamStore,
 * MIDI router and SynthCtl protocol the firmware exposes.
 *
 * ---------------------------------------------------------------------------
 * Why the init order is copied rather than reasoned about again
 *
 * main.cpp's sequence is not alphabetical and not arbitrary. drums_init()
 * precedes engines_init() because the sampler engine plays the drum bus's
 * pads; chord_init() follows seqarp_init() because chord.follow mirrors
 * seq.scale; presets_init() precedes drums_kits_load() because the kits'
 * fallback lives on the filesystem presets just mounted; looper_init() follows
 * both because it sizes its pool from what PSRAM is left; persist_init() is
 * last of the registration phase because it applies stored values through
 * ParamStore::set() and everything it can touch must already exist.
 *
 * Each of those is documented at its call site there. This file follows that
 * order exactly, and the place to change it is that file.
 *
 * ---------------------------------------------------------------------------
 * Threading
 *
 * osynth_host_start() must be called once, from one thread, before anything
 * else here. It creates the same control tasks the firmware does (sequencer
 * clock, preset worker, looper control, persist) plus the audio thread, and
 * returns once they are running.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Where persistent data lives. NULL uses the platform default; an
     * embedding app should pass the directory its OS allows it to write --
     * on Android and iOS that is not guessable, which is why this is here.
     * See host_paths.h. */
    const char* data_dir;

    /* Budget for the pool that stands in for PSRAM, in bytes, and for the
     * one that stands in for internal RAM. 0 takes the defaults from
     * sdkconfig.h. The first is what the looper sizes its recording cap from,
     * so it is a promise about recording time -- see esp_heap_caps.h. */
    size_t spiram_budget;
    size_t internal_budget;

    /* Start the audio device. False brings the engine up silent, which is
     * what a test harness driving the protocol wants: everything answers,
     * nothing opens a device or holds a thread against a deadline. */
    bool start_audio;

    /* Open the host's MIDI input ports and merge them into the router, so a
     * connected keyboard plays the synth. See osynth_host_midi.h: it opens
     * every port it finds, and does nothing on a platform with no backend. */
    bool start_midi_in;
} osynth_host_config_t;

/* Fills `out` with the defaults. */
void osynth_host_config_default(osynth_host_config_t* out);

/* Brings the engine up. `cfg` may be NULL for the defaults. Calling twice
 * returns ESP_ERR_INVALID_STATE without touching anything. */
esp_err_t osynth_host_start(const osynth_host_config_t* cfg);

/* Releases the audio device. The control tasks keep running: they own state
 * the app may still be reading, and nothing in the firmware ever stops them
 * either. Safe to call when start was never called or already stopped. */
void osynth_host_stop(void);

/* True between a successful start and a stop. */
bool osynth_host_running(void);

#ifdef __cplusplus
}
#endif
