/*
 * osynth — application entry point.
 * Boot order: codec mute (S31e; two I2C writes before anything else, so an
 * ES8388 is not driving its outputs unconfigured for the length of the boot)
 * -> banner -> NVS -> global params -> voice manager (S4; owns the
 * render callback, registers the 0x01xx params) -> mod matrix (S9; 0x05xx
 * slot params — all parameter registration must precede the audio task,
 * whose matrix plan reads the registry) -> drum / sample bus (S22, S44;
 * 0x07xx params, the factory kit and the recorder — ahead of the engines
 * because the S44 sampler engine plays its pads, and ahead of seq/arp so its
 * drum lanes can see the kit's slot count) -> engines (S5/S6; registers the
 * active engine's 0x02xx params, binds it, starts the engine-switch task)
 * -> FX bus (S10; 0x03xx params + delay-line allocation, before the audio
 * task for the same registration-race reason) -> seq/arp (S12/S23; 0x04xx params + the pattern store
 * + the 96 PPQN clock task, before the audio task for the same
 * reason — it also hooks the MIDI router's note tap, which is safe before
 * midi_init: the taps are plain function pointers) -> presets (S13; the
 * 0x000x trigger params + LittleFS mount + preset task, before the audio
 * task for the same registration rule) -> persisted settings (S25; restores
 * saved values, so it runs after every registration and before the audio
 * task) -> USB (S3; must also
 * precede the audio task, whose USB sink feeds it — and S35: the persisted
 * `usb.mode` decides here whether that is the device stack or the MIDI host,
 * which is why it has to come after persist_init) -> audio task (core 1,
 * render chain: voice sum -> drums -> FX bus -> looper) -> codec (S31b; the
 * ES8388's I2C bring-up, *after* the audio task because that is what starts
 * the MCLK it needs — a no-op on a discrete front end)
 * -> the working state (S40; restores the engine, the patch, the graph and
 * the sequencer the synth was switched off with — after the audio task
 * because a restore may switch engines, and before BLE so the app connects
 * to a settled synth)
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
#include "chord.h"
#include "codec.h"
#include "drums.h"
#include "engines.h"
#include "fx.h"
#include "local_ui.h"
#include "looper.h"
#include "midi.h"
#include "persist.h"
#include "presets.h"
#include "sampler.h"
#include "seqarp.h"
#include "synth_config.h"
#include "synth_mod.h"
#include "synth_params.h"
#include "synth_voice.h"
#include "synth_warn.h"
#include "usb_dev.h"
#include "usb_host_midi.h"

static const char* TAG = "osynth";

using namespace osynth;

static void register_global_params() {
    /* Order and count must track synth_engine_type_t, which since S38 is
     * unconditional — "modular" is named here even on a build without it,
     * because its index is reserved either way (engines.h explains why). An
     * app that offers it on such a build gets the switch task's revert and
     * an EVT_ENGINE saying the engine did not change; the app's own engine
     * list gates it on GRAPH_INFO and does not offer it at all. */
    static const char* kEngineNames[] = {"subtractive", "additive", "fm",
                                         "wavetable", "modular", "granular",
                                         "sampler"};
    static_assert(sizeof(kEngineNames) / sizeof(kEngineNames[0]) ==
                      SYNTH_ENGINE_COUNT,
                  "engine.type names must track synth_engine_type_t");
#if SYNTH_ENABLE_AUDIO_IN
    /* Where the input joins the render chain, and therefore what it records:
     * `mon` is mixed after the looper's record tap, so it is heard but never
     * printed into a take; `fx` and `dry` are mixed before it. See
     * render_chain() below. */
    static const char* kInRouteNames[] = {"off", "mon", "fx", "dry"};
#endif
#if SYNTH_ENABLE_IN_SOURCE_SEL
    /* Which device feeds that route (S37). Registered only where both are
     * compiled in — see SYNTH_ENABLE_IN_SOURCE_SEL — because a selector with
     * one position is a control the app would draw and no one could use.
     *
     * `both` is last so the two single-device values keep the numbers they
     * were persisted with, and so a stored 0 still means `line` — which is
     * what every build before the microphone existed was doing. */
    static const char* kInSourceNames[] = {"line", "mic", "both"};
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
#if SYNTH_ENABLE_USB_HOST
    /* Which role the one OTG port takes at boot (S35). Registered only where
     * both roles are reachable — on a build where the USB sink is the audio
     * clock there is no honest second option, and an enum whose second entry
     * silences the synth is worse than no control at all. */
    static const char* kUsbModeNames[] = {"device", "host"};
#endif
    static const ParamDesc kGlobals[] = {
        {PID_MASTER_VOLUME, "master.volume", ParamType::Float,
         ParamCurve::Linear, 0.0f, 1.0f, 0.8f, nullptr, 0},
        {PID_ENGINE_TYPE, "engine.type", ParamType::Enum, ParamCurve::Linear,
         0.0f, (float)(SYNTH_ENGINE_COUNT - 1), (float)SYNTH_ENGINE_SUBTRACTIVE,
         kEngineNames, SYNTH_ENGINE_COUNT},
#if SYNTH_ENABLE_AUDIO_IN
        {PID_LINE_IN_ROUTE, "in.route", ParamType::Enum, ParamCurve::Linear,
         0.0f, 3.0f, 0.0f, kInRouteNames, 4},
        /* Linear, not Exp: smooth_exp() divides by its running value, so an
         * exponential curve needs min > 0 — and 0 here has to mean silent. */
        {PID_LINE_IN_GAIN, "in.gain", ParamType::Float, ParamCurve::Linear,
         0.0f, 4.0f, 1.0f, nullptr, 0},
#endif
#if SYNTH_ENABLE_IN_SOURCE_SEL
        /* Defaults to `line`, and that default is the whole compatibility
         * story: presets save sparsely, so every patch written before this
         * parameter existed is read back as whichever value sits here. `line`
         * is the source every such patch was played through. */
        {PID_LINE_IN_SOURCE, "in.source", ParamType::Enum, ParamCurve::Linear,
         0.0f, 2.0f, 0.0f, kInSourceNames, 3},
        /* The mic's own trim, which is what makes `both` usable rather than
         * merely possible: the two devices do not arrive anywhere near each
         * other in level, so one shared in.gain across the pair is a control
         * that is wrong for one of them whichever way it is set. Same range and
         * curve as in.gain, and Linear for the same reason — 0 has to mean
         * silent, and smooth_exp() cannot start there. */
        {PID_LINE_IN_MICGAIN, "in.micgain", ParamType::Float,
         ParamCurve::Linear, 0.0f, 4.0f, 1.0f, nullptr, 0},
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
#if SYNTH_ENABLE_USB_HOST
        /* Default device: that is what osynth has always been, and the mode
         * that needs no extra hardware to be useful. Writing this changes
         * nothing until the next boot — the port cannot swap roles live — so
         * the app pairs it with an explicit restart (OP_REBOOT). */
        {PID_USB_MODE, "usb.mode", ParamType::Enum, ParamCurve::Linear, 0.0f,
         1.0f, (float)USB_MODE_DEVICE, kUsbModeNames, 2},
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
#if SYNTH_ENABLE_AUDIO_IN
        /* Same reasoning: the route and the trim describe what is plugged
         * into the box, not what patch is loaded. Presets skip both. */
        PID_LINE_IN_ROUTE,
        PID_LINE_IN_GAIN,
#endif
#if SYNTH_ENABLE_IN_SOURCE_SEL
        /* And so does which socket it is plugged into, and how much the
         * microphone in it needs lifting. Both describe the rig, not the
         * patch. */
        PID_LINE_IN_SOURCE,
        PID_LINE_IN_MICGAIN,
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
#if SYNTH_ENABLE_USB_HOST
        /* The whole point of the setting: it is read back at the *next* boot,
         * before either USB stack starts, and nothing else would remember it
         * across the restart that applying it requires. */
        PID_USB_MODE,
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
 * `in.route` selects carries gain; the other two return on a compare.
 *
 * The chain below is written as three pieces rather than one function (S45),
 * because the P4 runs it as a two-core pipeline and the alternative was two
 * copies of the order. Everything about *what* runs and in what sequence lives
 * in the three chain_* helpers; the entry points after them add only the cycle
 * markers and, where the pipeline is compiled in, the stage boundary.
 *
 * The boundary is between chain_voices() and chain_pre_fx(), and it is the
 * only place in the chain where it could be. Everywhere else something
 * downstream writes state that something upstream reads back inside the same
 * block: drums_pre_fx() renders into a scratch buffer that drums_post_fx() and
 * the FX bus compressor's key tap (drums_block_hit()) both read, and the three
 * input mix points have to agree with what audio_io_in_fx_block() hands the
 * noise-reduction units down to the last multiply. All of that stays whole on
 * the bus stage; what crosses to the voice stage is a piece with one output and
 * no readers. */
static inline void SYNTH_RENDER_IRAM chain_voices(float* l, float* r,
                                                  size_t frames, void* ctx) {
    voice_manager_render(l, r, frames, ctx);
}

static inline void SYNTH_RENDER_IRAM chain_pre_fx(float* l, float* r,
                                                  size_t frames) {
    drums_pre_fx(l, r, frames);
    audio_io_line_in_fx(l, r, frames);
}

static inline void SYNTH_RENDER_IRAM chain_post_fx(float* l, float* r,
                                                   size_t frames) {
    drums_post_fx(l, r, frames);
    audio_io_line_in_dry(l, r, frames);
    looper_process(l, r, frames);
    /* After the record tap: monitored, never printed into a take. */
    audio_io_line_in_mon(l, r, frames);
    /* The sampler's capture point (S44), and its placement *is* the definition
     * of `smp.src = bus`: after the looper, so resampling captures the loops
     * that are playing, and before the metronome, so a count-in never ends up
     * inside the sample it was counting in. Both are the same two reasons the
     * looper's own record tap sits where it does. */
    sampler_capture(l, r, frames);
    /* After the looper on purpose: the metronome is monitoring, not material,
     * and mixing it earlier printed count-in ticks into the take. */
    drums_render_click(l, r, frames);
}

#if SYNTH_ENABLE_SPLIT_RENDER

/* The voice stage, one block ahead of the sink. */
static void SYNTH_RENDER_IRAM render_stage_a(float* out_l, float* out_r,
                                             size_t frames, void* ctx) {
    const uint32_t c0 = esp_cpu_get_cycle_count();
    chain_voices(out_l, out_r, frames, ctx);
    /* The one piece of state that has to cross the cut: the note-start tap the
     * vocoder retriggers on, produced here and read by the FX bus a block
     * later. Inside the measured window because it is part of the stage's
     * work, and after chain_voices() because that is what fills it. */
    voice_manager_stage_block_note();
    audio_io_report_stage_voices(esp_cpu_get_cycle_count() - c0);
}

/* The bus stage, on the block the voice stage finished one period ago, and
 * then the sink.
 *
 * `voi` on the heartbeat therefore means voices *only* here, where on a
 * single-core build it also carries the drum bus and the input mix. That is
 * the meter following the hardware rather than drifting from it: the three
 * numbers now read as voice stage | bus stage, which is the split that says
 * which of the two to take load off. */
static void SYNTH_RENDER_IRAM render_stage_b(float* out_l, float* out_r,
                                             size_t frames, void* ctx) {
    (void)ctx; /* the voice stage got it; nothing downstream needs one */
    const uint32_t c1 = esp_cpu_get_cycle_count();
    /* Before anything in this stage can ask for it, and in particular before
     * fx_process(): this is what makes voice_manager_block_note() answer for
     * the block being finished rather than the one the voice stage is
     * building. */
    voice_manager_take_block_note();
    chain_pre_fx(out_l, out_r, frames);
    fx_process(out_l, out_r, frames);
    const uint32_t c2 = esp_cpu_get_cycle_count();
    chain_post_fx(out_l, out_r, frames);
    const uint32_t c3 = esp_cpu_get_cycle_count();
    audio_io_report_stage_fx_loop(c2 - c1, c3 - c2);
}

#else

static void SYNTH_RENDER_IRAM render_chain(float* out_l, float* out_r,
                                           size_t frames, void* ctx) {
    /* Cycle-counter reads so the heartbeat can attribute the load to a stage
     * (S21b). Chasing "clicks with underruns" from one total number is
     * guesswork — voices, drums, FX and looper have very different cost
     * curves (polyphony vs hit density vs always-on vs track count). The
     * drum bus is folded into the voices figure: both scale with how much is
     * sounding, and a fourth number would not have changed any diagnosis so
     * far. The input mix is folded in with it, for the same reason — it would
     * never have earned a number of its own. */
    const uint32_t c0 = esp_cpu_get_cycle_count();
    chain_voices(out_l, out_r, frames, ctx);
    chain_pre_fx(out_l, out_r, frames);
    const uint32_t c1 = esp_cpu_get_cycle_count();
    fx_process(out_l, out_r, frames);
    const uint32_t c2 = esp_cpu_get_cycle_count();
    chain_post_fx(out_l, out_r, frames);
    const uint32_t c3 = esp_cpu_get_cycle_count();
    audio_io_report_stages(c1 - c0, c2 - c1, c3 - c2);
}

#endif /* SYNTH_ENABLE_SPLIT_RENDER */

/* One place for "start the audio engine": which entry point that is depends on
 * the build, and the call itself appears twice below — either side of
 * codec_init(), for the OSYNTH_CODEC_INIT_BEFORE_I2S A/B. */
static esp_err_t start_audio(void) {
#if SYNTH_ENABLE_SPLIT_RENDER
    return audio_io_start_split(render_stage_a, render_stage_b, nullptr);
#else
    return audio_io_start(render_chain, nullptr);
#endif
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
             "target %s | usb:%d usb_host:%d ble:%d i2s_dac:%d line_in:%d "
             "mic_in:%d local_ui:%d serial_midi:%d",
             CONFIG_IDF_TARGET, SYNTH_ENABLE_USB, SYNTH_ENABLE_USB_HOST,
             SYNTH_ENABLE_BLE, SYNTH_ENABLE_I2S_DAC, SYNTH_ENABLE_LINE_IN,
             SYNTH_ENABLE_MIC_IN, SYNTH_ENABLE_LOCAL_UI,
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
    /* Ahead of engines_init() since S44, where it used to sit two lines below.
     * The sampler engine plays this component's pads, so binding it at boot —
     * which happens whenever `engine.type` was left there — would otherwise
     * run an engine init against a drum bus with no kit, no mu-law table and
     * no parameters yet. Nothing in the drum bus needs an engine, so the
     * dependency only ever ran one way and the old order was simply the order
     * these two were written in. */
    ESP_ERROR_CHECK(drums_init());         /* + drum/sampler params (0x07xx), factory kit */
    ESP_ERROR_CHECK(engines_init());       /* + engine params (0x02xx), binds engine */
    ESP_ERROR_CHECK(fx_init());            /* + FX-bus params (0x03xx), delay lines */
    ESP_ERROR_CHECK(seqarp_init());        /* + seq/arp params (0x04xx), pattern store, clock task */
    /* After seqarp: chord.follow mirrors seq.scale/seq.root, and chord mode
     * resolves those two ids at init. Before midi_init(), like every other
     * router hook: registering one is a pointer assignment, and the router
     * is not yet delivering anything for it to answer. */
    ESP_ERROR_CHECK(chord_init());         /* + chord params (0x044x), strum task */
    ESP_ERROR_CHECK(presets_init());       /* + preset params (0x000x), littlefs, task */
    /* Between these two on purpose (S44): the sample kits' no-card fallback
     * lives on the LittleFS partition presets just mounted, and the PSRAM they
     * claim has to be gone before the looper measures the free pool to size
     * its loop cap. drums.cpp's drums_kits_load() carries the full reasoning. */
    drums_kits_load();                     /* recordable kit contents (S44) */
    ESP_ERROR_CHECK(looper_init());        /* + looper params (0x06xx), loop_ctl task */
    /* Last of the registration phase: it applies stored values through
     * ParamStore::set(), so everything it can touch must already exist. */
    ESP_ERROR_CHECK(persist_init());       /* restores saved settings (S25) */
    ParamStore::instance().dump();

    /* One OTG port, one role (S35). usb_mode_resolve() reads the `usb.mode`
     * parameter persist_init() just restored and latches the answer for this
     * boot, clamping to device where hosting is not supported. Both stacks are
     * compiled in — the choice is a runtime one — but only one starts, and
     * starting the wrong one is not a recoverable state, which is why this
     * sits above the audio task rather than anywhere more convenient: the USB
     * sink feeds usb_dev, and on a USB-only build the clamp guarantees
     * usb_dev is what came up. */
    if (usb_mode_resolve() == USB_MODE_HOST) {
        /* Not ESP_ERROR_CHECKed: a host stack that fails to install leaves a
         * synth with no USB MIDI in, which is worth an error in the log and a
         * BLE status the app can show — not a bootloop on a box whose audio,
         * BLE and DIN MIDI are all fine. */
        const esp_err_t uerr = usb_host_midi_init();
        if (uerr != ESP_OK) {
            ESP_LOGE(TAG, "USB host failed to start: %s (no USB MIDI in)",
                     esp_err_to_name(uerr));
        }
    } else {
        ESP_ERROR_CHECK(usb_dev_init());
    }

#if !OSYNTH_CODEC_INIT_BEFORE_I2S
    ESP_ERROR_CHECK(start_audio());
#endif

    /* Deliberately not ESP_ERROR_CHECKed — a codec that fails to answer leaves
     * the board silent, which is worth an error in the log but not a bootloop.
     * Whether this lands before or after the port is the open question
     * OSYNTH_CODEC_INIT_BEFORE_I2S exists to A/B; see codec.h. Either way it
     * has to be after persist_init(), because it applies in.pga and out.level
     * from the ParamStore — which is why the mute alone runs at the top of
     * this function instead. */
    (void)codec_init();

#if OSYNTH_ES8311_INIT_BEFORE_I2S
    (void)codec_mic_init();
#endif

#if OSYNTH_CODEC_INIT_BEFORE_I2S
    ESP_ERROR_CHECK(start_audio());
#endif

#if !OSYNTH_ES8311_INIT_BEFORE_I2S
    /* The microphone codec, *after* the port in both orderings — which is what
     * this placement is for, since the branch above means "after codec_init()"
     * and "after the port" are not the same place.
     *
     * It needs MCLK running. The ES8311's clock manager is configured from the
     * MCLK pin (that is what `use_mclk` means, and why the board routes one to
     * GPIO13 at all), and every reference driver — Espressif's esp_codec_dev,
     * ESPHome's component — writes that configuration with the clock already
     * present. Configuring it beforehand, which is what this call did when it
     * sat next to codec_init(), leaves the chip to reach its operating point
     * when the clock arrives rather than under the writes that set it, and the
     * symptom is a codec that reports a clean init and converts silence.
     *
     * That is the opposite of what the ES8388 next door wants on this board,
     * and the two are not in conflict: OSYNTH_CODEC_INIT_BEFORE_I2S is about
     * the *control bus* going deaf while the main port clocks pins adjacent to
     * it, and this is about a *converter* needing its clock. Different
     * question, different port, different answer — see
     * OSYNTH_ES8311_INIT_BEFORE_I2S in codec.h for the switch that puts this
     * back if the bus turns out to refuse it here too. */
    (void)codec_mic_init();
#endif
    /* The second pad reading, and the one that carries information: the sweep
     * inside audio_io_start() happens before any input codec has been told
     * anything, so a data pin standing still there is the expected result. If
     * it is still standing still here, configuring the converter changed
     * nothing on the wire. */
    audio_io_mic_probe_pads();
    ESP_LOGI(TAG, "audio sink: %s | codec: %s", audio_io_sink_name(),
             codec_name());

    /* The working state (S40): the box comes back as it was switched off.
     *
     * Here, and not up with the other init calls, because applying it can
     * switch engines — and the S6 switch protocol hands the voice pool over
     * on two render boundaries, so before the audio task there is no boundary
     * to hand over on and the switch is refused. Still ahead of BLE, so the
     * app connects to a synth that has already settled rather than watching
     * the whole parameter set move underneath its first read.
     *
     * Queued, not blocking: the restore runs on the preset task and the rest
     * of this function has nothing it can disturb. Nothing is auto-saved
     * until it has finished (presets.h). */
    /* Not ESP_ERROR_CHECKed. The only way this fails is a full request queue,
     * and a synth that refuses to boot because it could not arrange to restore
     * a patch is worse than one that boots at its defaults — the same
     * sink-fallback rule persist_init() follows. */
    const esp_err_t rerr = presets_state_restore();
    if (rerr != ESP_OK) {
        ESP_LOGW(TAG, "working state not restored: %s (starting at defaults)",
                 esp_err_to_name(rerr));
    }

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
    char in_seg[224]; /* route + three gains (S31), source tag (S37), raw
                      * slot bits (S37d). The
                      * starve counter is unbounded and sits at the end, so
                      * the margin is deliberate: truncating this line would
                      * take out exactly the number that says the input is
                      * not clocking. */
    char sink_seg[64];
    char pipe_seg[32]; /* the voice stage falling behind (S45) */
    /* Deadline misses as of the previous beat, for the warning below. The
     * first beat only records the baseline: the blocks either side of
     * codec_init() reliably miss a few, that is a boot transient rather than
     * a patch being too expensive, and a warning that fires on every boot is
     * a warning nobody reads by the time it matters. */
    uint32_t last_underruns = 0;
    bool underrun_primed = false;
    bool underrun_warned = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SYNTH_HEARTBEAT_MS));
        /* Anything the render path wanted to say since the last beat. It
         * cannot say it itself — a console write from the audio task is a
         * dropout (synth_warn.h) — so this is where those lines appear. */
        osynth::dsp::render_warn_drain();
        audio_io_get_stats(&st);
        const synth_engine_t* eng = engines_get(engines_active_type());
        /* Appended only when the sink has actually refused a block, so a
         * healthy board's heartbeat stays the length it has always been.
         * Worth separating from `underruns` beside it: this count climbing
         * while that one stays flat means the sink is rejecting blocks the
         * render produced on time, which is a different fault from the render
         * missing its deadline and used to look identical from here. */
        if (st.sink_errors != 0) {
            snprintf(sink_seg, sizeof(sink_seg), " | SINK ERR %u (%s)",
                     (unsigned)st.sink_errors,
                     esp_err_to_name((esp_err_t)st.sink_last_err));
        } else {
            sink_seg[0] = '\0';
        }

        /* Appended on the same terms and for the same reason (S45): a healthy
         * board never prints it, and a board that does has a fault the
         * underrun count beside it cannot express. An underrun is a core that
         * ran out of *budget*; a stall is the bus stage with nothing to work
         * on at all, because the voice stage never delivered — starved of
         * scheduling rather than of DSP. The output is silence either way,
         * which is exactly why the two need separating from here.
         *
         * An over-budget voice stage is the *other* one: it delivers late, not
         * never, so it shows up in `underruns` and a dsp figure near 100 while
         * this stays at zero. */
        if (st.pipe_stalls != 0) {
            snprintf(pipe_seg, sizeof(pipe_seg), " | PIPE STALL %u",
                     (unsigned)st.pipe_stalls);
        } else {
            pipe_seg[0] = '\0';
        }

        /* The render chain missed deadlines during this window.
         *
         * This is the backstop for what graph_compile.h's budget structurally
         * cannot check. That budget prices a patch against a reservation
         * table, and two entries in it are known not to be worst cases — the
         * FX bus is measured with its default units on rather than all
         * fourteen, and the drum bus is reserved at zero. What the switches
         * and the pattern will be after the patch is accepted is not a
         * property of the patch, so no compile-time check can see this
         * coming. The alternative to saying it here is the user diagnosing an
         * underrun by ear, which is the exact outcome the budget exists to
         * prevent.
         *
         * Said once per run, not once per beat: it is a standing condition,
         * and repeating it every second would push the log out of the window
         * where the rest of the diagnosis lives. The per-stage percentages on
         * the heartbeat line itself are what to read after it — they say
         * which stage to take the load out of. */
        if (st.underruns != last_underruns) {
            if (!underrun_primed) {
                underrun_primed = true;
            } else if (!underrun_warned) {
                underrun_warned = true;
                ESP_LOGW(TAG,
                         "render missed its deadline (%u blocks) — the bus is "
                         "over budget for what is switched on. Watch the "
                         "[voi fx loop] split on the line below; if a modular "
                         "patch is loaded, its cost was priced against "
                         "graph_compile.h's reservation table, which does not "
                         "cover a fully loaded FX bus or a busy drum kit.",
                         (unsigned)(st.underruns - last_underruns));
            }
            last_underruns = st.underruns;
        }
#if SYNTH_ENABLE_USB
        /* Occupancy of the USB EP-IN FIFO as a percentage of its depth, over
         * the heartbeat window. The class driver steers this toward 50% by
         * varying the isochronous packet size, which is how the render clock
         * and the host's frame clock stay reconciled without resampling — so
         * a band around 50% is the healthy reading, and one pinned at 100%
         * with `drop` climbing is the stream falling behind. Pegged at 100%
         * with no drops is normal on a USB-only build, where this FIFO paces
         * the audio task rather than following it (see usb_dev.h). */
        if (usb_mode_active() == USB_MODE_HOST) {
            /* No FIFO to report in host mode — there is no outgoing audio
             * stream at all. What matters instead is whether anything was
             * found on the bus, which is the difference between a wiring
             * problem and a firmware one. */
            usb_host_midi_info_t hi;
            usb_host_midi_get_info(&hi);
            snprintf(usb_seg, sizeof(usb_seg), " | usb host %u dev",
                     (unsigned)hi.attached);
        } else {
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
        }
#else
        usb_seg[0] = '\0';
#endif
#if SYNTH_ENABLE_AUDIO_IN
        /* Pre-gain peak, so it reads what the converter saw: clipping in
         * front of it is analogue and no trim in here undoes it. `starve`
         * must stay at 0 — anything else means that device's RX side is not
         * clocking, or that it never came up at all. There is one counter per
         * device, and a device that is present but unselected still reports:
         * the capture reads every device every block whatever `in.source`
         * says, so these stay meaningful before you switch to anything.
         *
         * `route` and the three gains after it are the audio task's own view,
         * and they are here because the peak alone cannot say where the block
         * went — see the in_route note in audio_io.h. Read them as: the named
         * route should be the one the app is showing, and its gain should be
         * the only non-zero of the three. `mon` being the live one is the
         * answer to "the looper is not recording the input": that position is
         * mixed after the record tap by design. */
        int in_n = 0;
        static const char* kRouteSeg[] = {"off", "mon", "fx", "dry"};
        /* `fold` is the peak of (L+R)/2 — what a mono take actually stores. Far
         * below the two channel peaks means the input cancels when summed, so
         * the looper records silence from a source that meters and monitors
         * perfectly. See the in_peak_mono note in audio_io.h.
         *
         * A mono microphone is the opposite reading and equally correct: both
         * channels the same figure and `fold` matching them, because the slot
         * is duplicated rather than panned (source_mic.cpp). */
#if SYNTH_ENABLE_IN_SOURCE_SEL
        /* Both devices, every block, because both are captured every block —
         * an unselected microphone still meters, which is how you see one
         * working before switching to it. `g` after each pair of peaks is the
         * device's live mix gain: 0.00 means captured but not reaching the bus,
         * and two non-zero values mean `both` (or a crossfade still running).
         *
         * The mic's gain carries in.micgain folded in, so it is the number
         * that separates "the mic is silent" from "the mic is turned down" —
         * a distinction the peaks beside it cannot make, since they are
         * measured before any of it. */
        in_n = snprintf(in_seg, sizeof(in_seg),
                 " | in line %.2f/%.2f f%.2f g%.2f mic %.2f/%.2f f%.2f g%.2f "
                 "%s %.2f/%.2f/%.2f, starve %u/%u",
                 st.in_peak_l[0], st.in_peak_r[0], st.in_peak_mono[0],
                 st.in_dev_g[0], st.in_peak_l[1], st.in_peak_r[1],
                 st.in_peak_mono[1], st.in_dev_g[1],
                 kRouteSeg[st.in_route & 3], st.in_g[0], st.in_g[1],
                 st.in_g[2], (unsigned)st.in_starves[0],
                 (unsigned)st.in_starves[1]);
#else
        in_n = snprintf(in_seg, sizeof(in_seg),
                 " | in pk %.2f/%.2f fold %.2f %s g %.2f/%.2f/%.2f, starve %u",
                 st.in_peak_l[0], st.in_peak_r[0], st.in_peak_mono[0],
                 kRouteSeg[st.in_route & 3], st.in_g[0], st.in_g[1],
                 st.in_g[2], (unsigned)st.in_starves[0]);
#endif
#if SYNTH_ENABLE_MIC_IN
        /* The raw microphone slots, before narrowing and before any gain:
         * every bit either slot carried during the window. `raw 000000/000000`
         * means the data pin never left zero for a whole second — a fault in
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
        in_seg[0] = '\0';
#endif
        ESP_LOGI(TAG,
                 "alive | heap free %u (min %u) | audio blocks %u, underruns %u, "
                 "dsp %.1f%% (pk %.1f%%) [voi %.1f fx %.1f loop %.1f] | "
                 "out pk %.2f, sat %u | voices %u/%d (+%d drum) | engine %s | "
                 "ble %s%s%s%s%s",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)st.blocks_rendered, (unsigned)st.underruns,
                 st.dsp_load_pct, st.dsp_load_peak_pct,
                 st.stage_voices_pct, st.stage_fx_pct, st.stage_loop_pct,
                 st.out_peak, (unsigned)st.soft_clips,
                 (unsigned)voice_manager_active_voices(),
                 SYNTH_VOICES, drums_active_voices(),
                 eng != nullptr ? eng->name : "none",
                 ble_ctrl_state_name(), usb_seg, in_seg, sink_seg, pipe_seg);
    }
#endif
}
