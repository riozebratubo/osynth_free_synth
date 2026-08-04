/*
 * osynth — application entry point.
 * Boot order: codec mute (S31e; two I2C writes before anything else, so an
 * ES8388 is not driving its outputs unconfigured for the length of the boot)
 * -> banner -> NVS -> global params -> voice manager (S4; owns the
 * render callback, registers the 0x01xx params) -> mod matrix (S9; 0x05xx
 * slot params — all parameter registration must precede the audio task,
 * whose matrix plan reads the registry) -> engines (S5/S6; registers the
 * active engine's 0x02xx params, binds it, starts the engine-switch task)
 * -> FX bus (S10; 0x03xx params + delay-line allocation, before the audio
 * task for the same registration-race reason) -> drum bus (S22; 0x07xx
 * params + the factory kit, before seq/arp so its drum lanes can see the
 * kit's slot count) -> seq/arp (S12/S23; 0x04xx params + the pattern store
 * + the 96 PPQN clock task, before the audio task for the same
 * reason — it also hooks the MIDI router's note tap, which is safe before
 * midi_init: the taps are plain function pointers) -> presets (S13; the
 * 0x000x trigger params + LittleFS mount + preset task, before the audio
 * task for the same registration rule) -> persisted settings (S25; restores
 * saved values, so it runs after every registration and before the audio
 * task) -> USB device (S3; must also
 * precede the audio task, whose USB sink feeds it) -> audio task (core 1,
 * render chain: voice sum -> drums -> FX bus -> looper) -> codec (S31b; the
 * ES8388's I2C bring-up, *after* the audio task because that is what starts
 * the MCLK it needs — a no-op on a discrete front end)
 * -> midi (hooks the USB RX callback)
 * -> BLE control (S14; NimBLE + the SynthCtl GATT service — after the
 * audio task on purpose: it registers no params, and radio bring-up frees
 * nothing if it fails, so the synth core is already alive either way) ->
 * local UI (stub; hardware bring-up is future work) -> heartbeat loop.
 */
#include <cstdio>

#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#include "audio_io.h"
#include "ble_ctrl.h"
#include "codec.h"
#include "drums.h"
#include "engines.h"
#include "fx.h"
#include "local_ui.h"
#include "looper.h"
#include "midi.h"
#include "persist.h"
#include "presets.h"
#include "seqarp.h"
#include "synth_config.h"
#include "synth_mod.h"
#include "synth_params.h"
#include "synth_voice.h"
#include "usb_dev.h"

static const char* TAG = "osynth";

using namespace osynth;

static void register_global_params() {
    /* Order and count must track synth_engine_type_t — the modular engine
     * (S28) is Kconfig-gated, so on a build without it the enum stops at
     * wavetable and the app never offers a slot that cannot be bound. */
    static const char* kEngineNames[] = {"subtractive", "additive", "fm",
                                         "wavetable",
#if SYNTH_ENABLE_MODULAR
                                         "modular",
#endif
    };
#if SYNTH_ENABLE_LINE_IN
    /* Where the line input joins the render chain, and therefore what it
     * records: `mon` is mixed after the looper's record tap, so it is heard
     * but never printed into a take; `fx` and `dry` are mixed before it. See
     * render_chain() below. */
    static const char* kInRouteNames[] = {"off", "mon", "fx", "dry"};
#endif
#if SYNTH_ENABLE_CODEC_ES8388 && SYNTH_ENABLE_LINE_IN
    /* The ES8388's PGA moves in 3 dB steps and stops at 24. An enum of the
     * nine reachable settings beats an integer range the firmware would
     * silently round — the app then offers exactly what the hardware does,
     * and the stored value *is* the register code. */
    static const char* kInPgaNames[] = {"0 dB",  "+3 dB",  "+6 dB",
                                        "+9 dB", "+12 dB", "+15 dB",
                                        "+18 dB", "+21 dB", "+24 dB"};
#endif
    static const ParamDesc kGlobals[] = {
        {PID_MASTER_VOLUME, "master.volume", ParamType::Float,
         ParamCurve::Linear, 0.0f, 1.0f, 0.8f, nullptr, 0},
        {PID_ENGINE_TYPE, "engine.type", ParamType::Enum, ParamCurve::Linear,
         0.0f, (float)(SYNTH_ENGINE_COUNT - 1), (float)SYNTH_ENGINE_SUBTRACTIVE,
         kEngineNames, SYNTH_ENGINE_COUNT},
#if SYNTH_ENABLE_LINE_IN
        {PID_LINE_IN_ROUTE, "in.route", ParamType::Enum, ParamCurve::Linear,
         0.0f, 3.0f, 0.0f, kInRouteNames, 4},
        /* Linear, not Exp: smooth_exp() divides by its running value, so an
         * exponential curve needs min > 0 — and 0 here has to mean silent. */
        {PID_LINE_IN_GAIN, "in.gain", ParamType::Float, ParamCurve::Linear,
         0.0f, 4.0f, 1.0f, nullptr, 0},
#endif
#if SYNTH_ENABLE_CODEC_ES8388 && SYNTH_ENABLE_LINE_IN
        /* Analogue gain, ahead of the converter — the one that actually buys
         * headroom, since anything clipped in front of the ADC is gone. Only
         * registered where the hardware has such a stage; `in.gain` above is
         * the digital trim and means the same thing on every front end. */
        {PID_LINE_IN_PGA, "in.pga", ParamType::Enum, ParamCurve::Linear, 0.0f,
         8.0f, 0.0f, kInPgaNames, 9},
#endif
#if SYNTH_ENABLE_CODEC_ES8388
        /* Analogue output driver level in dB, quantised to the hardware's
         * 1.5 dB steps on the way to the register. A float and not an enum of
         * the 34 reachable settings, which is what in.pga got: PARAM_INFO is
         * a single frame and drops enum names that no longer fit (ble_ctrl.cpp),
         * so 34 of them would arrive at the app silently truncated. Rounding
         * by at most 0.75 dB on a trim that is set once by ear is the smaller
         * lie, and the app shows real dB either way.
         *
         * This is deliberately not master.volume. That one stays digital and
         * slewed per block; this register steps 1.5 dB with no ramp and would
         * click on every step of a drag. Set it for the load — 0 dB into a
         * line input, lower for headphones — and ride master.volume. */
        {PID_OUT_LEVEL, "out.level", ParamType::Float, ParamCurve::Linear,
         -45.0f, 4.5f, 0.0f, nullptr, 0},
#endif
    };
    ParamStore::instance().add(kGlobals, sizeof(kGlobals) / sizeof(kGlobals[0]));

    /* Settings that should survive a power cycle (S25). Master volume is the
     * room's gain, not part of any patch — the preset system deliberately
     * skips it — so nothing else was ever going to remember it.
     *
     * Add more ids here, or call persist_add() from the owning component's
     * init. Only ids with a stable meaning: the 0x02xx engine range is
     * re-registered per engine, so an id there means something different
     * depending on what is bound, and patch data belongs in presets anyway. */
    static const uint16_t kPersisted[] = {
        PID_MASTER_VOLUME,
#if SYNTH_ENABLE_LINE_IN
        /* Same reasoning: the route and the trim describe what is plugged
         * into the box, not what patch is loaded. Presets skip both. */
        PID_LINE_IN_ROUTE,
        PID_LINE_IN_GAIN,
#endif
#if SYNTH_ENABLE_CODEC_ES8388 && SYNTH_ENABLE_LINE_IN
        PID_LINE_IN_PGA,
#endif
#if SYNTH_ENABLE_CODEC_ES8388
        /* What is plugged into the output jack does not change with the
         * patch, and coming back from a power cycle at line level into
         * headphones would be unpleasant. */
        PID_OUT_LEVEL,
#endif
    };
    persist_add(kPersisted, sizeof(kPersisted) / sizeof(kPersisted[0]));
}

/* Audio-task render chain: voice sum -> drum bus (FX send) -> FX bus ->
 * drum bus (dry remainder) -> looper (record tap + track playback; S15).
 * audio_io applies master volume and the int16 conversion after, so
 * everything stays inside the DSP-load meter.
 *
 * The drum bus (S22) renders once, into its own scratch, and is added to the
 * main bus in two portions around the FX bus: `drums.send` of it before,
 * the rest after. A drum bus hard-wired through a reverb tuned for a pad is
 * unusable, and one hard-wired dry cannot sit in a mix — this way the same
 * FX bus serves both without a second set of effects.
 *
 * The looper still sits last so a track captures the live synth *and* the
 * drums with their FX print, and the played-back tracks are neither
 * re-recorded nor re-effected.
 *
 * The line input (S31) is one capture with three possible destinations, and
 * the looper's record tap is what makes them different. `fx` joins before the
 * FX bus, so external gear is reverberated and that print lands in the take.
 * `dry` joins after it, recorded without effects. `mon` joins after the
 * looper entirely — heard, never recorded, which is what makes it safe to
 * leave a live microphone monitored while looping. Only the position
 * `in.route` selects carries gain; the other two return on a compare. */
static void SYNTH_RENDER_IRAM render_chain(float* out_l, float* out_r,
                                           size_t frames, void* ctx) {
    /* Cycle-counter reads so the heartbeat can attribute the load to a stage
     * (S21b). Chasing "clicks with underruns" from one total number is
     * guesswork — voices, drums, FX and looper have very different cost
     * curves (polyphony vs hit density vs always-on vs track count). The
     * drum bus is folded into the voices figure: both scale with how much is
     * sounding, and a fourth number would not have changed any diagnosis so
     * far. */
    const uint32_t c0 = esp_cpu_get_cycle_count();
    voice_manager_render(out_l, out_r, frames, ctx);
    drums_pre_fx(out_l, out_r, frames);
    /* Before the c1 marker, so the input's cost folds into `voi` alongside
     * the drums rather than earning a fourth number nobody would read. */
    audio_io_line_in_fx(out_l, out_r, frames);
    const uint32_t c1 = esp_cpu_get_cycle_count();
    fx_process(out_l, out_r, frames);
    const uint32_t c2 = esp_cpu_get_cycle_count();
    drums_post_fx(out_l, out_r, frames);
    audio_io_line_in_dry(out_l, out_r, frames);
    looper_process(out_l, out_r, frames);
    /* After the record tap: monitored, never printed into a take. */
    audio_io_line_in_mon(out_l, out_r, frames);
    /* After the looper on purpose: the metronome is monitoring, not material,
     * and mixing it earlier printed count-in ticks into the take. */
    drums_render_click(out_l, out_r, frames);
    const uint32_t c3 = esp_cpu_get_cycle_count();
    audio_io_report_stages(c1 - c0, c2 - c1, c3 - c2);
}

extern "C" void app_main(void) {
    /* First, ahead of the banner: an ES8388 powers up with its output drivers
     * live and unconfigured, and everything below — NVS, ~250 parameter
     * registrations, the FX lines, the drum kit, the LittleFS mount, the
     * looper's PSRAM sizing — happens before codec_init() can do anything about
     * it. That gap is the scratch heard at power-on. Muting the DAC and
     * dropping the drivers takes one bus open and two register writes, needs
     * nothing that has been initialised yet, and leaves the chip in the state
     * codec_init()'s table starts from anyway. Ignored on failure: a board with
     * no codec has nothing to mute, and codec_init() is where that gets
     * reported properly. */
    (void)codec_early_mute();

    ESP_LOGI(TAG, "osynth v0.1.0 — multi-engine synthesizer");
    ESP_LOGI(TAG,
             "target %s | usb:%d ble:%d i2s_dac:%d line_in:%d local_ui:%d "
             "serial_midi:%d",
             CONFIG_IDF_TARGET, SYNTH_ENABLE_USB, SYNTH_ENABLE_BLE,
             SYNTH_ENABLE_I2S_DAC, SYNTH_ENABLE_LINE_IN, SYNTH_ENABLE_LOCAL_UI,
             SYNTH_ENABLE_SERIAL_MIDI);
    ESP_LOGI(TAG, "audio %d Hz, block %d, %d voices", SYNTH_SAMPLE_RATE,
             SYNTH_BLOCK_SIZE, SYNTH_VOICES);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "PSRAM: %u bytes", (unsigned)esp_psram_get_size());
#else
    ESP_LOGI(TAG, "PSRAM: not configured on this target");
#endif

    register_global_params();

    ESP_ERROR_CHECK(voice_manager_init()); /* + engine-common params (0x01xx) */
    ESP_ERROR_CHECK(synth_mod_init());     /* + mod-matrix params (0x05xx) */
    ESP_ERROR_CHECK(engines_init());       /* + engine params (0x02xx), binds engine */
    ESP_ERROR_CHECK(fx_init());            /* + FX-bus params (0x03xx), delay lines */
    ESP_ERROR_CHECK(drums_init());         /* + drum params (0x07xx), factory kit */
    ESP_ERROR_CHECK(seqarp_init());        /* + seq/arp params (0x04xx), pattern store, clock task */
    ESP_ERROR_CHECK(presets_init());       /* + preset params (0x000x), littlefs, task */
    ESP_ERROR_CHECK(looper_init());        /* + looper params (0x06xx), loop_ctl task */
    /* Last of the registration phase: it applies stored values through
     * ParamStore::set(), so everything it can touch must already exist. */
    ESP_ERROR_CHECK(persist_init());       /* restores saved settings (S25) */
    ParamStore::instance().dump();

    ESP_ERROR_CHECK(usb_dev_init());

    /* Before the port starts, on purpose: the codec's control bus shares the
     * connector with MCLK and BCLK, and it is only reliably quiet while those
     * are stopped. It also has to be after persist_init(), because it applies
     * in.pga and out.level from the ParamStore — which is the reason the mute
     * alone runs at the top of this function instead. Deliberately not
     * ESP_ERROR_CHECKed — a codec that fails to answer leaves the board silent,
     * which is worth an error in the log but not a bootloop. See codec.h for
     * what moved and why. */
    (void)codec_init();

    ESP_ERROR_CHECK(audio_io_start(render_chain, nullptr));
    ESP_LOGI(TAG, "audio sink: %s | codec: %s", audio_io_sink_name(),
             codec_name());

    ESP_ERROR_CHECK(midi_init());
    ESP_ERROR_CHECK(ble_ctrl_init());
    ESP_ERROR_CHECK(local_ui_init());

    ESP_LOGI(TAG, "init complete");

#if SYNTH_HEARTBEAT_MS == 0
    ESP_LOGI(TAG, "heartbeat disabled (OSYNTH_HEARTBEAT_MS = 0)");
    vTaskDelete(nullptr);
#else
    audio_io_stats_t st;
    char usb_seg[40];
    char in_seg[40];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SYNTH_HEARTBEAT_MS));
        audio_io_get_stats(&st);
        const synth_engine_t* eng = engines_get(engines_active_type());
#if SYNTH_ENABLE_USB
        /* Occupancy of the USB EP-IN FIFO as a percentage of its depth, over
         * the heartbeat window. The class driver steers this toward 50% by
         * varying the isochronous packet size, which is how the render clock
         * and the host's frame clock stay reconciled without resampling — so
         * a band around 50% is the healthy reading, and one pinned at 100%
         * with `drop` climbing is the stream falling behind. Pegged at 100%
         * with no drops is normal on a USB-only build, where this FIFO paces
         * the audio task rather than following it (see usb_dev.h). */
        usb_dev_audio_health_t uh;
        usb_dev_audio_get_health(&uh);
        if (uh.streaming && uh.fifo_depth != 0) {
            snprintf(usb_seg, sizeof(usb_seg), " | usb %u-%u%% drop %u",
                     (unsigned)(uh.fifo_min * 100u / uh.fifo_depth),
                     (unsigned)(uh.fifo_max * 100u / uh.fifo_depth),
                     (unsigned)uh.dropped_blocks);
        } else {
            snprintf(usb_seg, sizeof(usb_seg), " | usb idle");
        }
#else
        usb_seg[0] = '\0';
#endif
#if SYNTH_ENABLE_LINE_IN
        /* Pre-gain peak, so it reads what the ADC saw: clipping in front of
         * it is analogue and no trim in here undoes it. `starve` must stay
         * at 0 — anything else means the RX side is not clocking. */
        snprintf(in_seg, sizeof(in_seg), " | in pk %.2f/%.2f, starve %u",
                 st.in_peak_l, st.in_peak_r, (unsigned)st.in_starves);
#else
        in_seg[0] = '\0';
#endif
        ESP_LOGI(TAG,
                 "alive | heap free %u (min %u) | audio blocks %u, underruns %u, "
                 "dsp %.1f%% (pk %.1f%%) [voi %.1f fx %.1f loop %.1f] | "
                 "out pk %.2f, sat %u | voices %u/%d (+%d drum) | engine %s | "
                 "ble %s%s%s",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)st.blocks_rendered, (unsigned)st.underruns,
                 st.dsp_load_pct, st.dsp_load_peak_pct,
                 st.stage_voices_pct, st.stage_fx_pct, st.stage_loop_pct,
                 st.out_peak, (unsigned)st.soft_clips,
                 (unsigned)voice_manager_active_voices(),
                 SYNTH_VOICES, drums_active_voices(),
                 eng != nullptr ? eng->name : "none",
                 ble_ctrl_state_name(), usb_seg, in_seg);
    }
#endif
}
