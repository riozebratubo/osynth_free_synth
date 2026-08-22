/*
 * osynth — preset system (Session 13; banks widened in S33): named parameter
 * snapshots on LittleFS. 48 factory presets per engine (const, in flash) +
 * 64 user slots per engine on the 1 MB "storage" partition, plus 8 sequence
 * slots for the S12 recorder's 32 steps.
 *
 * Addressing is linear: slot = engine * 112 + index, index 0-47 factory /
 * 48-111 user (subtractive 0-111, additive 112-223, fm 224-335, wavetable
 * 336-447, modular 448-559). Slot 0 of every bank is the init patch (pure
 * defaults).
 *
 * A preset stores sparse {param id, value} overrides on the registered
 * defaults; loading resets the patch ranges first, then applies them, and
 * rides the S6 engine-switch protocol when the target bank's engine is not
 * the active one. State that is not part of a patch is never touched:
 * master.volume, engine.type (the load orchestrates it), the trigger
 * params below, seq.clock, and seq.mode/seq.steps (transport + the live
 * sequence, which the sequence slots own).
 *
 * Load/save run on the `preset` task (core 0); triggers only queue.
 * Trigger params (Int, min 0 — the NRPN data value is the slot number):
 *   preset.load        (0x0002)  write a slot number to load it
 *   preset.save        (0x0003)  write a user slot number to snapshot into it
 *   preset.seq.load    (0x0004)  sequence slots 0-7: one pattern each
 *   preset.seq.save    (0x0005)
 *   preset.seqset.load (0x0006)  set slots 0-7: the whole sequencer (S27)
 *   preset.seqset.save (0x0007)
 *
 * A *sequence* slot holds the pattern currently being edited. A *set* slot
 * holds everything the sequencer knows — every pattern with its tracks,
 * configuration and parameter locks, the song chain, and the 0x04xx
 * parameters that belong to an arrangement (tempo, feel, arpeggiator, track
 * mutes). Rig setup and transport state are not part of a set: seq.clock,
 * seq.mode, the playhead telemetry and the edit cursors stay put across a
 * load, exactly as they do across a preset load.
 *
 * ---------------------------------------------------------------------------
 * The working state (S40): "the box as you left it"
 *
 * Above the named slots sits one unnamed one, `/lfs/state.osw`, and nothing
 * ever asks for it. It is written by itself whenever the synth has been left
 * alone for a few seconds and the output has gone quiet, and it is read back
 * once at boot. Power the instrument off mid-session and it comes back on the
 * same engine, the same patch, the same drum kit, the same graph and the same
 * patterns — which is what a hardware groovebox does and what the named slots
 * alone could never do, because remembering to save is exactly what nobody
 * does before pulling the plug.
 *
 * It carries: every patch parameter (the preset rule below), plus the three
 * things a preset deliberately leaves alone because a *named* snapshot must
 * not move them behind the player — engine.type, drums.kit and seq.pattern —
 * plus the modular graph, plus the whole sequencer (every pattern and the
 * song chain), plus which preset slot was last loaded, so the app's "current
 * preset" readout survives the power cycle too.
 *
 * It deliberately does not carry:
 *   - anything persist (S25) already owns — master volume, the line input, the
 *     output level, the USB role. Those live in NVS with their own write
 *     policy, and two owners writing one setting from two files is a value
 *     that depends on which one lost the race. persist_owns() is the fence.
 *   - the transport (seq.mode) and the clock source (seq.clock). A synth that
 *     boots *playing*, or boots slaved to a clock that is not plugged in and
 *     therefore looks dead, is a trap — and it is the same trap the set slots
 *     document.
 *   - the looper (0x06xx). Minutes of audio is not a setting; the looper has
 *     its own explicit save, and auto-writing takes to flash would stall the
 *     render chain for as long as it took.
 *   - the momentary controls (voc.freeze, anr.learn, seq.fill) and every
 *     firmware-written telemetry value, for the reasons skip_id() lists.
 *
 * Writes are made rare and then made inaudible, by the same rules persist.h
 * sets out and for the same reason: dirty-marked rather than written,
 * settled, then held until audio_io reports silence, and skipped entirely
 * when the bytes would be identical to what is already on flash.
 *
 * `state.reset` (0x000F) is the way back: it puts everything the working
 * state covers back to its registered default, clears the sequencer and the
 * graph, and forgets the file — the state a first boot would have produced,
 * with the NVS settings above left exactly as they are.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* S33 widened the factory bank from 16 to 48 and moved the user range up to
 * match, so the 64 user slots are unchanged in *count*. They are not
 * unchanged in *number*: a user preset is a file named by its slot, and
 * `p<e>_16.osp` .. `p<e>_79.osp` are all below the new kUserFirst, so the
 * directory scan drops them. That is a deliberate one-time break — no
 * migration — and it means presets saved by pre-S33 firmware do not survive
 * the update. The files stay on flash; nothing reads them. */
#define PRESETS_PER_ENGINE    112 /* 0-47 factory, 48-111 user */
#define PRESETS_FACTORY_SLOTS 48
#define PRESETS_NAME_MAX      24 /* including the terminator */
#define PRESETS_SEQ_SLOTS     8
#define PRESETS_SET_SLOTS     8 /* whole-sequencer slots (S27) */

/* Trigger param ids (0x00xx global namespace) */
#define PRESET_PID_LOAD        0x0002
#define PRESET_PID_SAVE        0x0003
#define PRESET_PID_SEQ_LOAD    0x0004
#define PRESET_PID_SEQ_SAVE    0x0005
#define PRESET_PID_SEQSET_LOAD 0x0006
#define PRESET_PID_SEQSET_SAVE 0x0007
/* Write 1 to reset the synth to the state a first boot would have produced.
 * Int, min 0, and it snaps back to 0 when the reset has been performed, so an
 * app can watch it the way it watches the load/save triggers. */
#define PRESET_PID_STATE_RESET 0x000F

/* Mounts LittleFS on the "storage" partition (formats on first boot; a
 * mount failure degrades to factory-presets-only with a warning), registers
 * the trigger params and starts the preset task. Call before
 * audio_io_start(): every parameter registration must precede the audio
 * task (the S9 registry-read rule). */
esp_err_t presets_init(void);

/* Asynchronous request API (what the trigger params call internally; BLE
 * uses these in S14). Returns ESP_OK when queued — completion is logged.
 * `slot` is bank-relative (0..79 / 0..7); save requires a user slot in the
 * active engine's bank. `name` (save) is copied, NULL = "user <slot>". */
esp_err_t presets_request_load(int engine, int slot);
esp_err_t presets_request_save(int engine, int slot, const char* name);
esp_err_t presets_request_seq_load(int slot);
esp_err_t presets_request_seq_save(int slot);
/* Whole-sequencer slots (0..PRESETS_SET_SLOTS-1). A load replaces every
 * pattern, so patterns the file does not carry are cleared: what comes back
 * is the sequencer as it was saved, not a merge. */
esp_err_t presets_request_seqset_load(int slot);
esp_err_t presets_request_seqset_save(int slot);

/* ---- the working state ("current patch"; see the header comment) -------- */

/* Queues the boot restore. Call *after* audio_io_start(): applying it can
 * switch engines, and the S6 switch protocol's detach handshake needs the
 * audio task to be running blocks — before it there is no render boundary to
 * hand over on and the switch is refused. Returns ESP_OK when queued; the
 * result is logged, and a missing or unreadable file simply leaves the synth
 * at its registered defaults.
 *
 * Auto-saving starts when this completes, and not before: a restore that
 * marked its own writes dirty would spend the first quiet moment writing back
 * the file it had just read. */
esp_err_t presets_state_restore(void);

/* Writes the working state now if anything has moved, ignoring the settle and
 * silence waits. For a caller that knows a stall is acceptable — before a
 * deliberate reboot. Blocking; control tasks only. */
esp_err_t presets_state_save_now(void);

/* True if the slot holds a preset; copies its name out (both optional
 * outputs). Served from an in-RAM directory cache, so it is safe to call in a
 * loop from a latency-sensitive task (BLE LIST_PRESETS walks all 112 slots of
 * a bank). Only if the cache could not be allocated does it fall back to
 * reading the file header on the caller's task. */
bool presets_slot_info(int engine, int slot, char name[PRESETS_NAME_MAX],
                       bool* factory);

#ifdef __cplusplus
}
#endif
