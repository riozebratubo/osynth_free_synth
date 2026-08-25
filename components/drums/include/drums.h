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
 * Session 44 turned the one kit into a rack of them: the factory kit plus
 * OSYNTH_SAMPLE_KITS recordable user kits, every pad of which can be sampled
 * from the audio input or from the synth's own output (sampler.h).
 *
 * All of them are resident at once, which is a deliberate reversal of how this
 * component worked for twenty-odd sessions. Loading a kit on selection made
 * sense when there was one alternative and it lived on a card; it does not
 * survive the feature above, because it would mean a pad could only be
 * recorded into the kit that happened to be playing, and every kit change
 * would block the control task on SD I/O in the middle of a performance. So
 * user kits are read from storage once, at boot, into blocks they keep, and
 * selection is what it should have been all along: silence the voices, store
 * one pointer. Nothing is freed on a switch, which also removes the more
 * delicate half of the old swap protocol — what remains of it now guards a
 * single pad being republished rather than a whole kit being reclaimed.
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

#include "drum_kit_fmt.h"

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

/* Reads the recordable kits (S44) off storage into the tables drums_init()
 * built. A separate call because it has to sit *after* presets_init() — which
 * is what mounts the LittleFS partition the no-card fallback uses — and
 * *before* looper_init(), which sizes its loop cap from the PSRAM these kits
 * are about to claim. main.cpp is where that ordering is expressed. */
void drums_kits_load(void);

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

/* Number of selectable kits: 1 (factory) plus OSYNTH_SAMPLE_KITS recordable
 * user kits (S44). Every one of them is resident — see drums_kit_select().
 * drums_kit_select() is what the drums.kit parameter triggers. */
int drums_kit_count(void);
const char* drums_kit_name_at(int index);
esp_err_t drums_kit_select(int index);

/* Which kit is bound, and whether an index is one of the recordable ones.
 * Kit 0 is the factory kit: flash-mapped, read-only, and the one kit that can
 * neither be recorded into nor erased. Refusing that at the source is what
 * keeps "erase pad" from being a request to write to the XIP window. */
int drums_kit_index(void);
bool drums_kit_is_user(int index);

/* Renames a user kit. The name is what the app lists and what the sidecar
 * stores; it does not move the kit (see drum_kit.h on why kit N is a fixed
 * slot). */
esp_err_t drums_kit_rename(int index, const char* name);

/* Bumps whenever the bound kit changes *or* a pad in it is republished
 * (recorded over, erased, undone, copied into).
 *
 * This is the cross-component half of the rule drums.cpp's kit swap already
 * enforces internally: anything outside this component that latches a raw
 * sample pointer — the S44 sampler engine is the one such caller — must
 * compare this value at block start and drop what it is holding when it
 * moves. Two render boundaries after the bump, no voice anywhere can still be
 * pointing at the old block, which is what makes releasing it safe.
 * Any task. */
uint32_t drums_kit_generation(void);

/* A pad, snapshotted. Enough to start and run a voice without holding a
 * pointer into the kit structure itself, which is the whole reason it is a
 * copy: the kit may be republished under a voice that is already sounding,
 * and the generation counter above is how such a voice finds out. */
typedef struct {
    const uint8_t* data; /* nullptr when the pad is empty */
    uint32_t frames;
    uint32_t rate;
    uint32_t loop_start;
    uint32_t loop_end;
    float gain;
    float pan;
    float start_ofs; /* 0..1 into the sample */
    uint8_t format;
    uint8_t choke_group;
    uint8_t note;
    uint8_t play_mode; /* DRUM_PLAY_* — one-shot, gate or loop */
    uint8_t reverse;
} drums_pad_t;

/* Fills `out` from the currently bound kit. False (and `out` untouched) for an
 * out-of-range or empty pad. Safe from the audio task — one acquire load and a
 * struct copy, no locks. */
bool drums_pad_get(int slot, drums_pad_t* out);

/* The 256-entry G.711 mu-law decode table, built once by drums_init().
 *
 * Shared rather than rebuilt because the sampler engine has to decode the same
 * two formats this bus does — the factory kit is mu-law and every recorded pad
 * is PCM16 — and a second copy would be 512 bytes of initialised data in the
 * one region on the P4 that is actually short (see engine_sampler.h). The
 * decode itself is two lines at the call site, which is cheaper than a
 * function call per sample. Never null after drums_init(). */
const int16_t* drums_ulaw_table(void);

/* Releases any voice this slot is holding open. Only gate and loop pads hold
 * anything: a one-shot pad has no note-off, which is why drums_trigger() never
 * needed a partner until now. Harmless on a one-shot pad, so a control surface
 * can send it on every touch-up without asking what the pad is. */
void drums_release(int slot);

/* Editable per-pad fields (S44), addressed by index so one BLE opcode and one
 * app control can carry all of them. Append only: the index travels over the
 * wire and is written into a kit's sidecar. */
typedef enum {
    DRUM_PAD_FIELD_MODE = 0,   /* DRUM_PLAY_*                          */
    DRUM_PAD_FIELD_REVERSE = 1, /* 0 / 1                               */
    DRUM_PAD_FIELD_START = 2,  /* 0..1                                 */
    DRUM_PAD_FIELD_CHOKE = 3,  /* 0..7, 0 = none                       */
    DRUM_PAD_FIELD_NOTE = 4,   /* MIDI note the pad answers to         */
    DRUM_PAD_FIELD_GAIN = 5    /* baked trim, 0..4                     */
} drums_pad_field_t;

/* Edits one field of one pad of one kit (`kit` < 0 means the bound one).
 * Control task. Refuses the factory kit — it is flash-resident, and a kit that
 * cannot be saved must not appear to accept edits. */
esp_err_t drums_pad_set_field(int kit, int slot, drums_pad_field_t field,
                              float value);

/* Renames a pad. Separate from the field setter above because a name is not a
 * float, and widening that call to carry one would have made every numeric
 * edit pay for a string it never uses. The name is what the app labels the pad
 * with and what the WAV on the card is called after the next save. */
esp_err_t drums_pad_rename(int kit, int slot, const char* name);

/* Where user kits are being saved: "sd", "lfs" or "none". The app shows it,
 * and it is what decides whether a Save control is offered at all — a button
 * that cannot work is worse than a missing one. */
const char* drums_storage_name(void);

#ifdef __cplusplus
}
#endif
