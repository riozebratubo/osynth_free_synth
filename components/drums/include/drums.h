/*
 * osynth — drum / sample bus (Session 22).
 *
 * An always-on polyphonic one-shot sample player that renders *alongside*
 * the synth engine rather than replacing it: the sequencer can drive drums
 * and a synth patch in the same pattern, and a player can jam over a beat
 * without giving up the engine. That is the whole reason this is a separate
 * component and not a fifth entry in `engines` — binding an engine is
 * exclusive by construction (see the S6 switch protocol).
 *
 * Sound comes from a *kit*: a flat image described by drum_kit_fmt.h. The
 * factory kit is linked into the firmware as .rodata (built from a WAV pack
 * by tools/gen_drumkit.py) and is therefore flash-mapped — the voices read
 * const pointers and the kit costs zero RAM. On PSRAM targets, kits can also
 * be loaded from an SD card, either as the same `.okit` image or as a folder
 * of WAV files (see drum_kit.h).
 *
 * Placement in the render chain is split so the drum bus gets its own FX
 * send: drums_pre_fx() renders the voices into an internal scratch buffer
 * and adds the `drums.send` portion to the main bus *before* the FX bus, and
 * drums_post_fx() adds the remainder *after* it. A drum bus hard-wired
 * through a reverb tuned for a pad is unusable; one hard-wired dry can't sit
 * in a mix. See main.cpp's render_chain().
 *
 * Parameters live in 0x07xx (docs/PARAM_MAP.md). Slot parameters are named
 * generically (`drum1.level` …) rather than after the kit's samples: kits
 * are swappable at runtime, and a parameter called `kick.level` that plays a
 * conga after a kit change would be a lie. Human-readable slot names come
 * from drums_slot_name(), which the app shows instead.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Slots exposed as parameters. A kit may declare fewer (the extra slots are
 * silent); it may not declare more than DRUM_KIT_MAX_SLOTS. */
#define DRUM_SLOTS 16

/* Drum parameter IDs (0x07xx) — names, ranges and defaults in
 * docs/PARAM_MAP.md. */
#define DRUM_PID_LEVEL  0x0700 /* drums.level  float 0..1, bus gain          */
#define DRUM_PID_SEND   0x0701 /* drums.send   float 0..1, portion into FX   */
#define DRUM_PID_CHOKE  0x0702 /* drums.choke  bool, honour choke groups     */
#define DRUM_PID_KIT    0x0703 /* drums.kit    int, 0 = factory, 1.. = SD    */
#define DRUM_PID_TRIG   0x0704 /* drums.trig   int, trigger: audition a slot */
#define DRUM_PID_MIDICH 0x0705 /* drums.midich int 0..16, 0 = off, 10 = GM   */
#define DRUM_PID_CLICK  0x0706 /* drums.click  float 0..1, metronome level  */

/* Per-slot block of 4, slot s = 0..DRUM_SLOTS-1 → 0x0710..0x074F. */
#define DRUM_PID_SLOT_BASE     0x0710
#define DRUM_PID_SLOT_STRIDE   4
#define DRUM_PID_SLOT_LEVEL(s) (DRUM_PID_SLOT_BASE + (s) * DRUM_PID_SLOT_STRIDE + 0)
#define DRUM_PID_SLOT_PAN(s)   (DRUM_PID_SLOT_BASE + (s) * DRUM_PID_SLOT_STRIDE + 1)
#define DRUM_PID_SLOT_TUNE(s)  (DRUM_PID_SLOT_BASE + (s) * DRUM_PID_SLOT_STRIDE + 2)
#define DRUM_PID_SLOT_DECAY(s) (DRUM_PID_SLOT_BASE + (s) * DRUM_PID_SLOT_STRIDE + 3)

/* Registers the 0x07xx params, parses the embedded factory kit and allocates
 * the voice pool. Call before audio_io_start() — the S9 rule that every
 * registration precedes the audio task. Never fails hard: a missing or
 * corrupt kit logs a warning and leaves the bus silent (the sink-fallback
 * philosophy), because a synth that boots without drums is far better than
 * one that does not boot. */
esp_err_t drums_init(void);

/* Render + FX-send split; audio task only, no locks, no allocation.
 * The metronome is the exception to that split: drums_render_click() is
 * called after the *looper*, not just after the FX bus, so a count-in is
 * monitored but never recorded into a take.
 *
 * The metronome is the exception to that split: drums_render_click() is
 * called after the *looper*, not just after the FX bus, so a count-in is
 * monitored but never recorded into a take.
 *
 * drums_pre_fx() must be called before fx_process() and drums_post_fx()
 * after it, with the same frame count. */
void drums_pre_fx(float* l, float* r, size_t frames);
void drums_post_fx(float* l, float* r, size_t frames);

/* Mixes the metronome click. Must be called *after* the looper, so a count-in
 * is heard but never recorded. Cheap: it only touches the buffer while a
 * click is decaying (~45 ms). */
void drums_render_click(float* l, float* r, size_t frames);

/* Metronome click, used by the sequencer's and the looper's count-in and by
 * any future click track. Synthesised rather than sampled: a count-in has to
 * work before a kit is loaded, on a build with no kit at all, and while the
 * kit is being swapped. Rendered dry (after the FX bus), because a count-in
 * washed in reverb is harder to play to, not easier.
 * `accent` marks beat 1 of the bar — higher and louder. Any control task. */
void drums_click(bool accent);

/* Fire a slot. Velocity 1..127 (0 is ignored — a drum has no note-off).
 * Any control task: the request goes through a lock-free ring the audio task
 * drains at block start, exactly like the voice manager's note events.
 * `micro_frames` delays the hit by up to one block for sub-step timing;
 * pass 0 for "as soon as possible". */
void drums_trigger(int slot, int velocity, int micro_frames);

/* Sidechain key tap (S34): the velocity (1..127) at which `slot` *started a
 * voice* during the block currently being rendered, or 0 for "did not sound".
 * `delay_frames`, when not null, receives the hit's offset inside the block,
 * so a detector can place the impulse where the transient actually lands
 * rather than at the block boundary.
 *
 * Reports what sounded, not what was requested: a hit dropped by a full
 * trigger ring, aimed at an empty slot, or swallowed by a kit swap never
 * appears here. Several hits on one slot in one block report the loudest.
 *
 * Audio task only, and only from *after* drums_pre_fx() has run in the same
 * render callback — that is where it is filled in and where the previous
 * block's value is cleared. The FX bus compressor is the caller. */
uint8_t drums_block_hit(int slot, uint16_t* delay_frames);

/* MIDI note → slot, using the note map stored in the kit (a General-MIDI
 * drum map for the factory kit). Returns true if a slot claimed the note, so
 * the MIDI router can swallow it instead of playing it on the synth engine.
 * Only consults the map when drums.midich matches the incoming channel. */
bool drums_note_on(uint8_t channel, uint8_t note, uint8_t velocity);

/* MIDI note → slot through the current kit's note map, or -1 when no slot
 * claims that note. Used by sequencer drum lanes set to "note picks the
 * slot" instead of being bound to one slot. */
int drums_slot_for_note(uint8_t note);

/* The inverse: which MIDI note a slot answers to, or -1 for an empty slot.
 * The app uses it to label lanes and to play a slot from its keyboard. */
int drums_slot_note(int slot);

/* Kit introspection, for the BLE kit-info response and the local UI.
 * drums_slot_name() returns "" for a slot the current kit leaves empty. */
int drums_slot_count(void);
const char* drums_slot_name(int slot);
const char* drums_kit_name(void);
int drums_active_voices(void);

/* Number of selectable kits: 1 (factory) plus any found on the SD card.
 * drums_kit_select() is what the drums.kit parameter triggers; it runs on a
 * control task and may block on SD I/O. */
int drums_kit_count(void);
const char* drums_kit_name_at(int index);
esp_err_t drums_kit_select(int index);

#ifdef __cplusplus
}
#endif
