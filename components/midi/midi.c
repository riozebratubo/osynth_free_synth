/*
 * osynth — MIDI router (Session 4; sustain pedal + CC map in Session 5;
 * per-engine CC maps + program-change engine select in Session 6; NRPN +
 * mod-wheel-as-matrix-source in Session 9; seqarp note tap + real-time
 * routing in Session 12).
 *
 * USB-MIDI event packets arrive pre-framed from the TinyUSB task (core 0);
 * serial MIDI bytes are reassembled in midi_serial.c. Both funnel into
 * midi_route_channel_message(), which drives the voice manager. Omni mode:
 * the channel nibble is ignored (single-part synth).
 *
 * Since S9 the mod wheel (CC 1) no longer writes a vibrato-depth param —
 * it feeds the mod matrix `wheel` source (synth_mod.h), and NRPN
 * (CC 99/98 select a ParamStore id, CC 6/38 carry a 14-bit value) can set
 * *any* registered parameter — the general mapping layer the S5-S8
 * temporary CCs stood in for. The remaining CC tables below stay as live
 * conveniences until BLE control lands (S14).
 *
 * Per-event logging is debug-level only: the router runs on the USB task,
 * and a blocking UART log line there could starve the iso EP-IN refill in
 * tud_task (audible as a glitch while the host streams).
 */
#include "midi.h"

#include <stddef.h>

#include "esp_log.h"

#include "engine_additive.h"
#include "engine_fm.h"
#include "engine_granular.h"
#include "engine_subtractive.h"
#include "engine_wavetable.h"
#include "drums.h"
#include "engines.h"
#include "fx.h"
#include "midi_serial.h"
#include "synth_config.h"
#include "synth_mod.h"
#include "synth_params_c.h"
#include "synth_voice.h"
#include "usb_dev.h"
#include "usb_host_midi.h"

static const char* TAG = "midi";

/* seqarp hooks (S12): the note tap sees played note on/offs before the voice
 * manager (the arpeggiator consumes them while active; the sequencer records
 * them); the real-time callback receives clock/start/continue/stop. seqarp's
 * own emissions re-enter midi_route_channel_message() from its clock task
 * and pass the tap untouched (task-handle guard on the seqarp side). */
static midi_note_tap_fn s_note_tap = NULL;
static void* s_note_tap_ctx = NULL;
static midi_drum_tap_fn s_drum_tap = NULL;
static void* s_drum_tap_ctx = NULL;
static midi_realtime_fn s_realtime = NULL;
static void* s_realtime_ctx = NULL;
/* Chord mode's hook (S41). Null on a build without components/chord, and the
 * router then behaves exactly as it did before chord mode existed. */
static midi_chord_fn s_chord = NULL;
static void* s_chord_ctx = NULL;

void midi_set_note_tap(midi_note_tap_fn fn, void* ctx) {
    s_note_tap_ctx = ctx;
    s_note_tap = fn;
}

void midi_set_chord_hook(midi_chord_fn fn, void* ctx) {
    s_chord_ctx = ctx;
    s_chord = fn;
}

void midi_set_drum_tap(midi_drum_tap_fn fn, void* ctx) {
    s_drum_tap_ctx = ctx;
    s_drum_tap = fn;
}

void midi_set_realtime_callback(midi_realtime_fn fn, void* ctx) {
    s_realtime_ctx = ctx;
    s_realtime = fn;
}

static bool note_tap(uint8_t note, uint8_t velocity, bool on, int src) {
    const midi_note_tap_fn fn = s_note_tap;
    return fn != NULL && fn(note, velocity, on, src, s_note_tap_ctx);
}

static bool chord_hook(uint8_t note, uint8_t velocity, bool on, bool allow,
                       int when) {
    const midi_chord_fn fn = s_chord;
    return fn != NULL && fn(note, velocity, on, allow, when, s_chord_ctx);
}

void midi_play_note(uint8_t note, uint8_t velocity, bool on, bool pre,
                    int src) {
    if (on) {
        if (pre && note_tap(note, velocity, true, src)) return;
        voice_manager_note_on(note, velocity);
    } else {
        if (pre && note_tap(note, 0, false, src)) return;
        voice_manager_note_off(note);
    }
}

static void drum_tap(uint8_t note, uint8_t velocity) {
    const midi_drum_tap_fn fn = s_drum_tap;
    if (fn != NULL) fn(note, velocity, s_drum_tap_ctx);
}

/* Continuous controllers -> ParamStore, normalized through each param's
 * range and curve. Standard CC numbers where they exist; 9/14/15 are
 * "undefined" CCs pressed into service for unison testing until BLE control
 * lands (S14). The 0x02xx targets are per active engine — engines reuse the
 * same raw ids for unrelated params, so a single flat table would misroute
 * (S6 fix). Superseded as the general mapping layer by NRPN + the mod
 * matrix (S9) — these tables remain as live-tweak conveniences only. */
typedef struct {
    uint8_t cc;
    uint16_t pid;
} cc_entry_t;

static const cc_entry_t k_cc_common[] = {
    {5, SYNTH_PID_COMMON_GLIDE},      /* portamento time */
    {9, SYNTH_PID_COMMON_UNISON},
    {14, SYNTH_PID_COMMON_UNI_DETUNE},
    {15, SYNTH_PID_COMMON_UNI_SPREAD},
    /* The FX sends (S10-S11) use their standard CC meanings — permanent
     * rows, unlike the temporary test controls above/below. */
    {91, FX_PID_REV_MIX},             /* effects 1 depth: reverb */
    {92, FX_PID_CRUSH_MIX},           /* effects 2 depth -> bitcrush (S17) */
    {93, FX_PID_CHO_MIX},             /* effects 3 depth: chorus */
    {94, FX_PID_DLY_MIX},             /* effects 4 depth -> delay */
    {95, FX_PID_GRN_MIX},             /* effects 5 depth -> granular */
};

static const cc_entry_t k_cc_subtractive[] = {
    {71, SUB_PID_FLT_RESO},           /* "harmonic content" */
    {72, SUB_PID_ENV1_RELEASE},
    {73, SUB_PID_ENV1_ATTACK},
    {74, SUB_PID_FLT_CUTOFF},         /* "brightness" */
};

/* 74 is literally brightness here; 71 ("harmonic content") -> even/odd
 * balance; 70/75/76 are temporary test controls like the wavetable's:
 * tilt, inharmonicity and the brightness wobble have no other handle until
 * BLE lands (S14). The 16 drawbars wait for BLE too. */
static const cc_entry_t k_cc_additive[] = {
    {70, ADD_PID_TILT},               /* "sound variation" -> spectral tilt */
    {71, ADD_PID_EVENODD},            /* "harmonic content", literally */
    {72, ADD_PID_ENV1_RELEASE},
    {73, ADD_PID_ENV1_ATTACK},
    {74, ADD_PID_BRIGHT},             /* "brightness", literally */
    {75, ADD_PID_INHARM},
    {76, ADD_PID_LFO2_BRIGHT},
};

static const cc_entry_t k_cc_fm[] = {
    {71, FM_PID_A_FB},                /* "harmonic content" -> feedback */
    {72, FM_PID_A_ENV_R},
    {73, FM_PID_A_ENV_A},
    {74, FM_PID_A_INDEX},             /* "brightness" -> mod index */
};

/* 70/75/76 ("sound variation" / sound controllers, undefined defaults) are
 * temporary test controls like 9/14/15: position morph, table select and the
 * second oscillator have no other handle until BLE lands (S14). */
static const cc_entry_t k_cc_wavetable[] = {
    {70, WT_PID_OSC1_POS},            /* "sound variation" -> table position */
    {71, WT_PID_FLT_RESO},
    {72, WT_PID_ENV1_RELEASE},
    {73, WT_PID_ENV1_ATTACK},
    {74, WT_PID_FLT_CUTOFF},          /* "brightness" */
    {75, WT_PID_OSC1_TABLE},          /* 0/43/86/127 -> basic/sync/vocal/fm */
    {76, WT_PID_MIX_OSC2},            /* bring in wt2 (sync saw, +4 ct) */
};

/* 70/75/76/77 are the "sound variation / sound controller" block, undefined
 * by default and used here as they are for the wavetable engine: the cloud's
 * own shape controls have no other live handle. 74 is brightness and lands on
 * the filter cutoff as everywhere else — deliberately *not* on grn.form,
 * which sounds like a brightness control and is not one: it moves the formant
 * in ratio to the key, so a sweep of it is a vowel, not a tone control, and
 * one CC has to mean one thing across the engines. */
static const cc_entry_t k_cc_granular[] = {
    {70, GRAN_PID_FORM},              /* "sound variation" -> formant ratio */
    {71, GRAN_PID_FLT_RESO},          /* "harmonic content" */
    {72, GRAN_PID_ENV1_RELEASE},
    {73, GRAN_PID_ENV1_ATTACK},
    {74, GRAN_PID_FLT_CUTOFF},        /* "brightness" */
    {75, GRAN_PID_SIZE},              /* grain length */
    {76, GRAN_PID_SCAT},              /* pitch scatter: cloud vs train */
    {77, GRAN_PID_BUF_POS},           /* scrub the capture ring (src = in) */
};

#define CC_TABLE(t) t, (sizeof(t) / sizeof((t)[0]))

static bool cc_apply(const cc_entry_t* map, size_t len, uint8_t cc,
                     uint8_t value) {
    for (size_t i = 0; i < len; ++i) {
        if (map[i].cc == cc) {
            return synth_param_set_norm_midi(map[i].pid,
                                             (float)value * (1.0f / 127.0f));
        }
    }
    return false;
}

/* NRPN state (omni: one global selection). CC 99/98 select a parameter id
 * (MSB = id >> 7, LSB = id & 0x7F — ids fit 14 bits by construction),
 * CC 6/38 carry the 14-bit data value, applied on the LSB (38) so a partial
 * value never fires a listener (e.g. an engine switch via 0x0001). Senders
 * must emit the full 99/98/6/38 sequence (`sendmidi ... nrpn <id> <val>`
 * does). An RPN selection (CC 101/100) cancels the NRPN selection. */
static uint8_t s_nrpn_msb = 0x7F; /* 0x7F/0x7F: nothing selected */
static uint8_t s_nrpn_lsb = 0x7F;
static uint8_t s_nrpn_data_msb = 0;

static bool cc_route(uint8_t cc, uint8_t value) {
    if (cc_apply(CC_TABLE(k_cc_common), cc, value)) return true;
    switch (engines_active_type()) {
        case SYNTH_ENGINE_SUBTRACTIVE:
            return cc_apply(CC_TABLE(k_cc_subtractive), cc, value);
        case SYNTH_ENGINE_ADDITIVE:
            return cc_apply(CC_TABLE(k_cc_additive), cc, value);
        case SYNTH_ENGINE_FM:
            return cc_apply(CC_TABLE(k_cc_fm), cc, value);
        case SYNTH_ENGINE_WAVETABLE:
            return cc_apply(CC_TABLE(k_cc_wavetable), cc, value);
        case SYNTH_ENGINE_GRANULAR:
            return cc_apply(CC_TABLE(k_cc_granular), cc, value);
        default:
            /* The modular engine (S28) deliberately has no CC table: its
             * parameter ids are positional, so "CC 74 is the cutoff" has no
             * meaning when slot 3 may hold a filter today and an envelope
             * tomorrow. NRPN reaches every node parameter by id, which is
             * the addressing a graph can actually honour. */
            return false;
    }
}

void midi_route_channel_message(uint8_t status, uint8_t d1, uint8_t d2) {
    midi_route_note(status, d1, d2, true);
}

void midi_route_note(uint8_t status, uint8_t d1, uint8_t d2,
                     bool allow_chord) {
    d1 &= 0x7F;
    d2 &= 0x7F;
    switch (status & 0xF0) {
        case 0x90: /* note on (velocity 0 means note off) */
            if (d2 > 0) {
                ESP_LOGD(TAG, "note on  %u vel %u", d1, d2);
                /* The drum bus is the one place the channel nibble matters
                 * (S22): drums.midich picks a channel — 10 by convention —
                 * and notes on it address kit slots through the General-MIDI
                 * map instead of the synth engine. Everything else stays
                 * omni. Checked before the tap so a drum note never becomes
                 * arpeggiator input — but reported to the drum tap afterwards,
                 * because "not arpeggiator input" was never meant to imply
                 * "not recordable", and for a long time it silently did. */
                if (drums_note_on(status & 0x0F, d1, d2)) {
                    drum_tap(d1, d2);
                    break;
                }
                /* Chord mode (S41) is asked twice, and answers once. Before
                 * the tap it hands the arpeggiator the chord's tones; after
                 * it, it turns each note the arpeggiator plays into a block
                 * chord. `chord.route` picks which, so only one of these two
                 * calls can ever return true. */
                if (chord_hook(d1, d2, true, allow_chord, MIDI_CHORD_PRE)) {
                    break;
                }
                if (!note_tap(d1, d2, true, MIDI_NOTE_PLAYED)) {
                    if (!chord_hook(d1, d2, true, allow_chord,
                                    MIDI_CHORD_POST)) {
                        voice_manager_note_on(d1, d2);
                    }
                }
            } else {
                ESP_LOGD(TAG, "note off %u (vel 0)", d1);
                if (drums_note_on(status & 0x0F, d1, 0)) break;
                if (chord_hook(d1, 0, false, allow_chord, MIDI_CHORD_PRE)) {
                    break;
                }
                if (!note_tap(d1, 0, false, MIDI_NOTE_PLAYED)) {
                    if (!chord_hook(d1, 0, false, allow_chord,
                                    MIDI_CHORD_POST)) {
                        voice_manager_note_off(d1);
                    }
                }
            }
            break;
        case 0x80:
            ESP_LOGD(TAG, "note off %u", d1);
            if (drums_note_on(status & 0x0F, d1, 0)) break;
            if (chord_hook(d1, 0, false, allow_chord, MIDI_CHORD_PRE)) break;
            if (!note_tap(d1, 0, false, MIDI_NOTE_PLAYED)) {
                if (!chord_hook(d1, 0, false, allow_chord, MIDI_CHORD_POST)) {
                    voice_manager_note_off(d1);
                }
            }
            break;
        case 0xB0: /* control change */
            switch (d1) {
                case 1: /* mod wheel: the matrix `wheel` source (S9) */
                    synth_mod_set_wheel((float)d2 * (1.0f / 127.0f));
                    break;
                case 6:
                    /* NRPN data entry MSB: stored, applied on the LSB.
                     * Ignored while nothing is selected, so a stray CC 6 —
                     * an RPN sequence, a generic controller a DAW happens to
                     * map there — cannot sit in the latch and corrupt the top
                     * 7 bits of whatever NRPN write comes next. */
                    if (s_nrpn_msb != 0x7F || s_nrpn_lsb != 0x7F) {
                        s_nrpn_data_msb = d2;
                    }
                    break;
                case 38: { /* NRPN data entry LSB: apply the 14-bit value */
                    if (s_nrpn_msb == 0x7F && s_nrpn_lsb == 0x7F) break;
                    const uint16_t pid =
                        ((uint16_t)s_nrpn_msb << 7) | s_nrpn_lsb;
                    const uint16_t val =
                        ((uint16_t)s_nrpn_data_msb << 7) | d2;
                    if (synth_param_set_nrpn_midi(pid, val)) {
                        ESP_LOGD(TAG, "nrpn 0x%04x = %u", pid, val);
                    } else {
                        ESP_LOGD(TAG, "nrpn 0x%04x = %u (unregistered)", pid,
                                 val);
                    }
                    break;
                }
                case 64: /* sustain pedal */
                    voice_manager_set_sustain(d2 >= 64);
                    break;
                /* A fresh selection starts with a clean data latch, so a
                 * value can never inherit the MSB of the previous one. A
                 * sender that streams LSB-only updates (CC 38 alone) after a
                 * full sequence still works: only 98/99 clear it. */
                case 98: /* NRPN select LSB */
                    s_nrpn_lsb = d2;
                    s_nrpn_data_msb = 0;
                    break;
                case 99: /* NRPN select MSB */
                    s_nrpn_msb = d2;
                    s_nrpn_data_msb = 0;
                    break;
                case 100: /* RPN select: cancels any NRPN selection */
                case 101:
                    s_nrpn_msb = 0x7F;
                    s_nrpn_lsb = 0x7F;
                    s_nrpn_data_msb = 0;
                    break;
                case 120: /* all sound off */
                    voice_manager_all_sound_off();
                    /* Chord mode holds a table of keys and a reference count
                     * per sounding tone. The voices are gone either way, but
                     * leaving the table populated would strand those counts:
                     * the next press of a key still listed there would find
                     * its tones "already sounding" and emit nothing. */
                    chord_hook(0, 0, false, false, MIDI_CHORD_ALL_OFF);
                    break;
                case 121: /* reset all controllers */
                    voice_manager_set_pitch_bend(0.0f);
                    voice_manager_set_sustain(false);
                    synth_mod_set_wheel(0.0f);
                    break;
                case 123: /* all notes off */
                    voice_manager_all_notes_off();
                    chord_hook(0, 0, false, false, MIDI_CHORD_ALL_OFF);
                    break;
                default:
                    if (!cc_route(d1, d2)) {
                        ESP_LOGD(TAG, "cc %u = %u (unmapped)", d1, d2);
                    }
                    break;
            }
            break;
        case 0xC0: /* program change selects the engine (until BLE, S14) */
            if (d1 < SYNTH_ENGINE_COUNT) {
                ESP_LOGD(TAG, "program %u -> engine.type", d1);
                synth_param_set_midi(SYNTH_PID_ENGINE_TYPE, (float)d1);
            } else {
                ESP_LOGD(TAG, "program %u (no engine)", d1);
            }
            break;
        case 0xE0: { /* pitch bend: 14-bit, centered on 8192. Normalized to
                      * [-1, 1] here; the voice manager scales it by the
                      * common.bend.range parameter. */
            const int32_t raw = (int32_t)d1 | ((int32_t)d2 << 7);
            voice_manager_set_pitch_bend((float)(raw - 8192) *
                                         (1.0f / 8192.0f));
            break;
        }
        default: /* aftertouch: nothing to drive yet */
            break;
    }
}

void midi_route_realtime(uint8_t status) {
    switch (status) {
        case 0xF8: /* timing clock (24 PPQN) */
        case 0xFA: /* start */
        case 0xFB: /* continue */
        case 0xFC: /* stop */
            if (s_realtime != NULL) s_realtime(status, s_realtime_ctx);
            break;
        default: /* active sensing / system reset: nothing to drive */
            break;
    }
}

#if SYNTH_ENABLE_USB
/* One 4-byte USB-MIDI event packet, invoked on the TinyUSB task (device role)
 * or the host driver's client task (host role, S35). CINs 0x8..0xE carry
 * complete channel-voice messages; CIN 0xF is a single byte — System
 * Real-Time when >= 0xF8 (routed to the seqarp clock since S12). Everything
 * else (sysex, system common) is dropped here.
 *
 * Both roles share this function because they share the wire format: a
 * USB-MIDI 1.0 event packet is the same four bytes whichever end of the cable
 * produced it. That is the whole reason the host driver had nothing to parse. */
static void usb_midi_rx(const uint8_t packet[4], void* ctx) {
    (void)ctx;
    const uint8_t cin = packet[0] & 0x0F;
    if (cin >= 0x08 && cin <= 0x0E) {
        midi_route_channel_message(packet[1], packet[2], packet[3]);
    } else if (cin == 0x0F && packet[1] >= 0xF8) {
        midi_route_realtime(packet[1]);
    }
}
#endif

esp_err_t midi_init(void) {
#if SYNTH_ENABLE_USB
    /* Exactly one of these is live — the port took one role at boot — but
     * registering both costs two stores and keeps the decision in one place
     * (usb_mode_resolve, called by main.cpp). The stack that did not start
     * never calls back. */
    usb_dev_midi_set_rx_callback(usb_midi_rx, NULL);
    usb_host_midi_set_rx_callback(usb_midi_rx, NULL);
#endif

    esp_err_t err = midi_serial_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "serial MIDI failed to start: %s (continuing without)",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG,
             "router up: usb %d (%s), serial %d, omni, 8 common + per-engine "
             "CCs, wheel = matrix source, NRPN = any param, prog change = "
             "engine, realtime -> seq clock",
             SYNTH_ENABLE_USB,
             usb_mode_active() == USB_MODE_HOST ? "host" : "device",
             SYNTH_ENABLE_SERIAL_MIDI);
    return ESP_OK;
}
