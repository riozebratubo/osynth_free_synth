/*
 * osynth — preset system implementation (Session 13).
 *
 * Storage: LittleFS on the 1 MB `storage` partition, mounted at /lfs
 * (formatted on first boot; a failed mount degrades to factory-presets-only
 * with a warning — the sink-fallback philosophy). A user preset is one file
 * p<engine>_<slot>.osp: a 32-byte header (magic/version/engine/name) plus
 * sparse {u16 id, f32 value} pairs — only values that differ from their
 * registered defaults are stored, which keeps typical files small enough
 * for LittleFS inline storage. Factory presets are const pair tables in
 * flash (presets_factory.cpp). Sequence slots are s<n>.osq files holding a
 * whole S23 pattern — every track, its configuration and its parameter
 * locks; files written by S12-S22 firmware still load (seq_model.cpp
 * recognises the old 32-step layout). Set slots (S27) are set<n>.oss files
 * holding the *whole* sequencer: a header, the arrangement parameters, the
 * song chain and one pattern blob per pattern, in the same per-pattern
 * format the sequence slots use. Writes go to a temp file and rename() into
 * place, so a power cut mid-save can't corrupt an existing preset.
 *
 * Threading: the ParamStore listener (any control task — the USB task for
 * NRPN) only queues a request; the `preset` task does all file I/O and
 * parameter writes. A cross-engine load writes engine.type and polls until
 * the S6 switch task has bound the target engine before applying — the
 * registry is never mutated here (the S9 rule), values only land in
 * already-registered params. Every write this component makes carries
 * ParamOrigin::Preset, which the listener ignores: a stored snapshot can
 * never recursively trigger loads, and the bookkeeping writes that reflect
 * or revert the trigger params don't re-queue.
 *
 * The working state (S40) is a seventh file, /lfs/state.osw, that nobody
 * asks for: the preset task writes it whenever the synth has been left alone
 * for a few seconds and the output has gone quiet, and reads it back once at
 * boot. It is the patch plus the three things a *named* snapshot must not
 * move behind the player (engine.type, drums.kit, seq.pattern), plus the
 * graph, plus the whole sequencer. presets.h has the contract; state_id() has
 * the fence; state_tracks() explains why parameter locks are not edits.
 *
 * Apply = reset the registered patch params to defaults, then set the
 * stored pairs on top (unregistered ids are ignored — files from older
 * firmware stay loadable). Skipped in both directions, because they are
 * state rather than patch: master.volume (the room's gain), engine.type
 * (the load orchestrates it), the six trigger params, seq.clock
 * (external-sync setup), seq.mode/seq.steps and the sequencer's navigation
 * and telemetry params (transport + pattern data, owned by the sequence
 * slots), the whole looper range (0x06xx, S15 — transport, live track state
 * and mix levels) and the drum kit selection (0x07xx is otherwise stored:
 * the per-slot mixer is part of the sound).
 */
#include "presets.h"

#include <dirent.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_io.h" /* audio_io_quiet_ms(): when a flash write is inaudible */
#include "chord.h" /* CHORD_PID_*: skipped by patches, kept by the state (S41) */
#include "drums.h"
#include "engines.h"
#include "fx.h" /* FX_PID_*: the S36 enable-switch migration */
#include "persist.h" /* persist_owns(): the fence around the NVS settings */
#include "seq_model.h"
#include "seqarp.h"
#include "synth_config.h"
#include "synth_params.h"

#if SYNTH_ENABLE_MODULAR
#include "graph_model.h"
#endif

#include "presets_priv.h"

static const char* TAG = "presets";

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamOrigin;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

constexpr char kBasePath[] = "/lfs";
constexpr char kPartLabel[] = "storage";
constexpr uint32_t kPresetMagic = 0x3150534Fu; /* "OSP1" little-endian */
constexpr uint32_t kSetMagic = 0x3153534Fu;    /* "OSS1" little-endian */
constexpr uint16_t kSetVersion = 1;
constexpr uint32_t kStateMagic = 0x3157534Fu;  /* "OSW1" little-endian */
constexpr uint16_t kStateVersion = 1;

/* Working-state write policy. The same three rules persist.h sets out, and
 * for the same reason — a flash write parks the render chain — but with a
 * longer settle, because this write is kilobytes rather than a couple of
 * hundred bytes and there is no hurry about it. */
constexpr uint32_t kStateSettleMs = 5000;
constexpr uint32_t kStateQuietMs = 200;
constexpr uint32_t kStateMaxDeferMs = 180000;
constexpr uint32_t kStatePollMs = 500;

/* The same "wait for a gap in the audio" rule, applied to the writes the
 * player asks for by name rather than to the ones the firmware decides on.
 *
 * The background writes above have always been gated and the explicit ones
 * never were, which had it exactly backwards from the player's point of
 * view: an autosave that glitches is a mystery, but a glitch on *save preset*
 * is a glitch on the button you just pressed, while you were auditioning the
 * sound you are saving. The stall is not marginal either — the DAC has four
 * DMA buffers, 5.33 ms in total, and SPI_FLASH_ERASE_YIELD_DURATION_MS is 20,
 * so one erase chunk holds the cache down for about four times the whole
 * reservoir and parks core 1 with it.
 *
 * Short numbers, because unlike an autosave this one is *waiting on a person*
 * — the button has been pressed and nothing visible happens until the file is
 * written. 400 ms of patience buys the gap between two phrases; past that the
 * write goes ahead and takes its glitch, which is the right trade when the
 * alternative is a save that looks like it did not happen. looper.cpp refuses
 * outright in the same situation, but a looper save is megabytes and a preset
 * is kilobytes — deferring beats refusing at this size. */
constexpr uint32_t kSaveQuietMs = 120;
constexpr uint32_t kSaveWaitMs = 400;
constexpr uint32_t kSavePollMs = 20;
constexpr int kUserFirst = PRESETS_FACTORY_SLOTS;
constexpr int kSlotCount = SYNTH_ENGINE_COUNT * PRESETS_PER_ENGINE;
constexpr int kMaxPairs = (int)ParamStore::kMaxParams;

constexpr int kTaskPrio = 4; /* control plane, below eng_switch (5) */
constexpr int kTaskStack = 6144;
constexpr int kSwitchWaitMs = 2000;

/* version 1: header + `count` {id, value} pairs.
 * version 2 (S28): the same, then a modular graph blob — the node kinds and
 * cables that give the 0x02xx ids in the pairs their meaning. Written only
 * for the modular engine, so every other bank's files stay byte-identical
 * v1 and nothing already on the device is rewritten or invalidated.
 * version 3 / 4 (S36): the same two layouts again — 3 without a graph blob,
 * 4 with one — but written by firmware that has the per-effect enable
 * switches (fx.<unit>.on).
 *
 * The version bump buys exactly one thing, and it is not a layout change.
 * A .osp is sparse: do_save() writes only values that differ from their
 * default, and the switches default to off. So in a v3 file, "no `fx.rev.on`
 * pair" means the player bypassed the reverb — while in a v1 file it means
 * the firmware had no such parameter and the reverb was governed by its mix
 * alone. Identical bytes, opposite meanings, and no way to tell them apart
 * except by asking which firmware wrote them. That is what the version byte
 * now answers, and legacy_fx_enable() is what acts on the answer. */
struct __attribute__((packed)) PresetHdr {
    uint32_t magic;
    uint8_t version; /* 1, or 2 when a graph blob follows the pairs */
    uint8_t engine;
    uint16_t count;
    char name[PRESETS_NAME_MAX]; /* NUL-padded */
};
static_assert(sizeof(PresetHdr) == 32, "on-disk layout");
constexpr uint8_t kPresetVersionLegacy = 1;
constexpr uint8_t kPresetVersionLegacyGraph = 2;
constexpr uint8_t kPresetVersion = 3;
constexpr uint8_t kPresetVersionGraph = 4;

/* Whether this firmware can read `v` at all. Every version ever written is
 * still readable — the differences are all in interpretation, not layout. */
constexpr bool preset_version_known(uint8_t v) {
    return v == kPresetVersionLegacy || v == kPresetVersionLegacyGraph ||
           v == kPresetVersion || v == kPresetVersionGraph;
}

/* Whether `v` carries a modular graph blob after the pairs. */
constexpr bool preset_version_has_graph(uint8_t v) {
    return v == kPresetVersionLegacyGraph || v == kPresetVersionGraph;
}

/* Whether `v` predates the per-effect enable switches and therefore needs
 * legacy_fx_enable() run over it after its pairs land. */
constexpr bool preset_version_pre_fx_on(uint8_t v) {
    return v == kPresetVersionLegacy || v == kPresetVersionLegacyGraph;
}

/* Ceiling on the graph blob a *file* may carry, on every build — a firmware
 * without the modular engine still has to recognise one written by a firmware
 * with it, and step over it rather than reject the whole file. */
constexpr size_t kGraphBlobMax = 256;

#if SYNTH_ENABLE_MODULAR
/* Staging for the graph blob between fetch_snapshot() and do_load(). Both
 * run on the `preset` task, so a static is safe and saves ~130 bytes of
 * stack on a task that already carries the pairs array. */
uint8_t s_graph_blob[osynth::graph::kSerialMaxBytes];
static_assert(sizeof(s_graph_blob) <= kGraphBlobMax, "graph blob ceiling");
#endif
size_t s_graph_len = 0;

/* Set by fetch_snapshot() alongside the pairs: true when the source was a
 * .osp written before the S36 enable switches existed, and its effects
 * therefore have to be inferred from their mix values. Staged rather than
 * returned because fetch_snapshot() already returns the pair count, and a
 * second out-parameter for one bool on a two-call path is worse than the
 * static the graph blob next to it already uses. False for factory
 * presets, which name their switches explicitly. */
bool s_legacy_fx = false;

/* A set file: this header, then `params` pairs, then `song_len` chain
 * entries, then `patterns` pattern blobs each prefixed by its u32 length.
 * The blobs are exactly what a sequence slot stores, so the two formats
 * cannot drift apart. */
struct __attribute__((packed)) SetHdr {
    uint32_t magic;
    uint16_t version; /* 1 */
    uint8_t patterns;
    uint8_t song_len;
    uint16_t params;
    uint16_t reserved;
    char name[PRESETS_NAME_MAX]; /* NUL-padded */
};
static_assert(sizeof(SetHdr) == 36, "on-disk layout");

/* The working state file (S40): the header, then `params` pairs, then a
 * `graph_len`-byte modular graph blob, then `song_len` chain entries, then
 * `patterns` pattern blobs each prefixed by its u32 length, and last a
 * `chord_len`-byte user chord set (S41).
 *
 * Deliberately its own format rather than a preset plus a set: the two
 * overlap (both carry 0x04xx parameters) and neither carries engine.type,
 * which is the one value that has to be read before anything else can be
 * applied. Reusing the *sections* is what keeps them from drifting — the
 * pattern blobs are exactly what a sequence slot stores and the graph blob is
 * exactly what a v4 preset stores.
 *
 * The chord set went into the spare byte and onto the end rather than taking
 * a version bump, and that is worth a sentence: `version` is compared for
 * equality, so bumping it would have thrown away every stored working state
 * in the field — every user's sequencer, patch and engine choice — to add one
 * optional 96-byte section. A file written before S41 carries chord_len 0 and
 * simply has no section, which is exactly what "no set was stored" should
 * mean, and a pre-S41 firmware reading a new file stops after the patterns
 * and ignores the tail. */
struct __attribute__((packed)) StateHdr {
    uint32_t magic;
    uint16_t version;
    uint8_t engine;
    uint8_t patterns;
    uint16_t params;
    uint16_t graph_len;
    uint8_t song_len;
    uint8_t chord_len; /* 0 on files written before S41 */
    int16_t preset;    /* linear slot last loaded, -1 = none — display only */
};
static_assert(sizeof(StateHdr) == 16, "on-disk layout");

enum Op : uint8_t {
    OP_LOAD, OP_SAVE, OP_SEQ_LOAD, OP_SEQ_SAVE, OP_SET_LOAD, OP_SET_SAVE,
    OP_STATE_LOAD, OP_STATE_SAVE, OP_STATE_RESET
};

struct Req {
    uint8_t op;
    int16_t slot; /* linear preset slot, or sequence slot */
    char name[PRESETS_NAME_MAX];
};

QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;
bool s_fs_ok = false;
int s_cur_load = 0;         /* preset.load rests on the last loaded slot */
int s_cur_save = kUserFirst;

/* ---- working state (S40) ----
 *
 * s_state_ready gates the whole mechanism: it goes true when the boot restore
 * has finished, so nothing the restore itself writes can mark the state
 * dirty, and a build that never calls presets_state_restore() simply never
 * auto-saves. The dirty pair mirrors persist.cpp — a flag for "something
 * moved" and a counter for "and it is still moving", which is what the settle
 * timer watches. */
std::atomic<bool> s_state_ready{false};
std::atomic<bool> s_state_dirty{false};
std::atomic<uint32_t> s_state_seq{0};
/* Hash of the blob last written (or read at boot). The dirty flag only says a
 * parameter was *written*, not that its value moved — a knob dragged back to
 * where it started, a preset re-loaded, a step toggled twice — so without this
 * an idle session would still cost a flash write every few minutes. Cheaper
 * than persist.cpp's memcmp against a stored copy, because a copy of this blob
 * would be tens of kilobytes of RAM held for the lifetime of the synth. */
uint32_t s_state_hash = 0;
bool s_state_hash_valid = false;
/* Given by the preset task when a queued OP_STATE_SAVE has finished, so the
 * reboot path can wait for the bytes to be on flash. */
SemaphoreHandle_t s_state_done = nullptr;

/* User-slot directory cache.
 *
 * presets_slot_info() used to fopen() the slot's file on the caller's task,
 * so one LIST_PRESETS meant 64 LittleFS lookups — nearly all of them misses,
 * but a miss still walks the directory. That ran inline on the BLE control
 * task, which is single-threaded: the command queue behind it filled during
 * the app's connect burst and a frame was dropped. The cache makes the call a
 * memcpy. It is built from one directory scan at mount and updated in place by
 * do_save(), the only thing that ever writes a preset file.
 *
 * Factory slots are not in it — their names are const data already.
 *
 * 4 engines x 64 user slots x 25 B is ~6 KB, so it is allocated rather than
 * static, and a mount or allocation failure simply leaves it null: every
 * lookup then takes the old per-call read, which is slow but correct. */
struct SlotCache {
    bool used;
    char name[PRESETS_NAME_MAX];
};
constexpr int kUserPerEngine = PRESETS_PER_ENGINE - PRESETS_FACTORY_SLOTS;
constexpr int kCacheEntries = SYNTH_ENGINE_COUNT * kUserPerEngine;
SlotCache* s_cache = nullptr;
int s_cache_count = -1; /* user presets on disk; -1 = no cache */
/* do_save() writes on the preset task while BLE and the local UI read; the
 * critical section is a 24 B copy, in the house style (seq_model, drums). */
portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;

/* preset-task-only scratch (single consumer — no reentrancy) */
uint16_t s_ids[ParamStore::kMaxParams];
preset_pair_t s_pairs[ParamStore::kMaxParams];

const char* engine_name(int engine) {
    const synth_engine_t* e = engines_get((synth_engine_type_t)engine);
    return e != nullptr ? e->name : "?";
}

/* %03d since S33: user slots run to 111, and a mix of two- and three-digit
 * names in one directory sorts badly and reads worse. parse_preset_name()
 * uses strtol, so it accepts either width — but nothing writes the old one
 * any more, and the pre-S33 files it would match are below kUserFirst and
 * rejected on slot range regardless. */
void preset_path(char* out, size_t n, int engine, int slot) {
    snprintf(out, n, "%s/p%d_%03d.osp", kBasePath, engine, slot);
}

void seq_path(char* out, size_t n, int slot) {
    snprintf(out, n, "%s/s%d.osq", kBasePath, slot);
}

void set_path(char* out, size_t n, int slot) {
    snprintf(out, n, "%s/set%d.oss", kBasePath, slot);
}

/* ---- user-slot directory cache ---------------------------------------- */

/* Reads a user slot's header off the filesystem. False for a missing file and
 * for anything that is not a preset of this version, so a stray or truncated
 * file never shows up in a listing. */
bool read_slot_name(int engine, int slot, char name[PRESETS_NAME_MAX]) {
    char path[40];
    preset_path(path, sizeof(path), engine, slot);
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) return false;
    PresetHdr h;
    /* Every known version, not just v1. Hard-coding v1 here was a bug from
     * S28 onwards: do_save() writes v2 for the modular engine, so a modular
     * user preset loaded fine when asked for by number but disappeared from
     * the listing on the next boot, which reads as "my preset is gone". */
    const bool ok = fread(&h, sizeof(h), 1, fp) == 1 &&
                    h.magic == kPresetMagic && preset_version_known(h.version);
    fclose(fp);
    if (!ok) return false;
    if (name != nullptr) {
        h.name[PRESETS_NAME_MAX - 1] = '\0'; /* the file need not terminate it */
        strlcpy(name, h.name, PRESETS_NAME_MAX);
    }
    return true;
}

int cache_index(int engine, int slot) {
    return engine * kUserPerEngine + (slot - kUserFirst);
}

/* Both callers have already range-checked engine and slot. */
void cache_store(int engine, int slot, const char* name) {
    if (s_cache == nullptr) return;
    char copy[PRESETS_NAME_MAX];
    strlcpy(copy, name, sizeof(copy));
    SlotCache* e = &s_cache[cache_index(engine, slot)];
    taskENTER_CRITICAL(&s_cache_lock);
    const bool was_used = e->used;
    memcpy(e->name, copy, sizeof(copy));
    e->used = true;
    taskEXIT_CRITICAL(&s_cache_lock);
    if (!was_used && s_cache_count >= 0) ++s_cache_count;
}

bool cache_lookup(int engine, int slot, char name[PRESETS_NAME_MAX]) {
    const SlotCache* e = &s_cache[cache_index(engine, slot)];
    char copy[PRESETS_NAME_MAX];
    taskENTER_CRITICAL(&s_cache_lock);
    const bool used = e->used;
    if (used) memcpy(copy, e->name, sizeof(copy));
    taskEXIT_CRITICAL(&s_cache_lock);
    if (used && name != nullptr) memcpy(name, copy, sizeof(copy));
    return used;
}

/* "p<engine>_<slot>.osp". Hand-parsed rather than sscanf'd: sscanf reports a
 * match on the two numbers whatever follows them, so a sequence file would
 * pass, and %n (which would catch that) is not there under nano formatting. */
bool parse_preset_name(const char* fn, int* engine, int* slot) {
    if (fn[0] != 'p') return false;
    char* end = nullptr;
    const long e = strtol(fn + 1, &end, 10);
    if (end == fn + 1 || *end != '_') return false;
    const char* s = end + 1;
    const long sl = strtol(s, &end, 10);
    if (end == s || strcmp(end, ".osp") != 0) return false;
    if (e < 0 || e >= SYNTH_ENGINE_COUNT) return false;
    if (sl < kUserFirst || sl >= PRESETS_PER_ENGINE) return false;
    *engine = (int)e;
    *slot = (int)sl;
    return true;
}

/* One readdir pass; only files that exist cost a header read, so a board with
 * no user presets pays a single directory scan and nothing else. Runs once,
 * from presets_init(), before the audio task exists. */
void cache_build(void) {
    s_cache_count = 0;
    DIR* dir = opendir(kBasePath);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "cannot scan %s — listings fall back to per-slot reads",
                 kBasePath);
        free(s_cache);
        s_cache = nullptr;
        s_cache_count = -1;
        return;
    }
    const struct dirent* de = nullptr;
    while ((de = readdir(dir)) != nullptr) {
        int engine = 0, slot = 0;
        if (!parse_preset_name(de->d_name, &engine, &slot)) continue;
        char name[PRESETS_NAME_MAX];
        /* opened anyway: the name lives in the header, and the check there
         * is what keeps a half-written file out of the listing */
        if (!read_slot_name(engine, slot, name)) continue;
        cache_store(engine, slot, name);
    }
    closedir(dir);
}

/* State that is not part of a patch — see the file header. */
bool skip_id(uint16_t id) {
    /* Looper (0x06xx): transport, live track state and mix levels. */
    if (id >= osynth::PID_LOOPER_BASE && id < osynth::PID_DRUMS_BASE) {
        return true;
    }
    /* Chord mode (0x044x, S41): a performance setting, not a patch value —
     * the same class as master volume and the line input. Loading a preset
     * called "Bell" mid-set must not silently change what your keyboard plays
     * a triad of, and the reverse is worse still: chord mode is most useful
     * exactly while you are auditioning sounds, and a patch change that
     * switched it off every time would make it unusable there.
     *
     * It is in the *working state* instead (state_id() widens this by the
     * whole range), so it survives a power cycle like everything else about
     * how the box was left. */
    if (id >= CHORD_PID_FIRST && id <= CHORD_PID_LAST) return true;
    /* Drums (0x07xx): the per-slot mixer *is* part of the sound and is
     * stored, but the kit selection and the audition trigger are not — a
     * preset that silently swapped the kit (or fired a hit) on load would be
     * a surprise, and the kit list differs per device anyway. */
    switch (id) {
        case DRUM_PID_KIT:
        case DRUM_PID_TRIG:
        case osynth::PID_MASTER_VOLUME:
        case osynth::PID_ENGINE_TYPE:
        /* Line input (S31): the rig's wiring, like master volume. Loading a
         * preset that silently unmuted a live microphone — or moved its gain
         * — would be a genuinely bad surprise. */
        case osynth::PID_LINE_IN_ROUTE:
        case osynth::PID_LINE_IN_GAIN:
        case osynth::PID_LINE_IN_PGA:
        /* And which socket it is (S37), for the same reason and one more: a
         * preset that changed the source would put the input through a
         * mute-swap-unmute on load, so the surprise would be audible even
         * with the route at off on the way in. */
        case osynth::PID_LINE_IN_SOURCE:
        case osynth::PID_LINE_IN_MICGAIN:
        case osynth::PID_OUT_LEVEL:
        /* The vocoder's freeze (S38): a performance control, not a patch
         * value. The Hold-to-sample button leaves it *on* — holding a
         * captured vowel is what it is for — so any save made after using it
         * once would store "starts frozen", and a frozen vocoder whose
         * followers were reset on load has nothing to say until someone
         * presses the button again. Same class as SEQ_PID_FILL below.
         *
         * The granular engine's buf.freeze is the same kind of control and
         * cannot be listed here: 0x02xx means whatever the bound engine says,
         * and that id is `env1.decay` on the subtractive one. It is handled
         * inside the engine instead, by refusing to honour a freeze until the
         * ring has actually been filled. */
        case FX_PID_VOC_FREEZE:
        /* The adaptive NR's learn (S39): momentary, and the same class of
         * control as the freeze above. The Hold-to-learn button leaves it
         * *off*, so storing it would store nothing useful even when it worked
         * — but a preset saved with a finger on the button would come back
         * permanently sampling, which is a unit that never settles. */
        case FX_PID_ANR_LEARN:
        case PRESET_PID_LOAD:
        case PRESET_PID_SAVE:
        case PRESET_PID_SEQ_LOAD:
        case PRESET_PID_SEQ_SAVE:
        case PRESET_PID_SEQSET_LOAD:
        case PRESET_PID_SEQSET_SAVE:
        /* And the reset trigger (S40), for the same reason as the six above
         * and rather more urgently: a preset that reset the instrument on
         * load would erase the sequencer of whoever selected it. */
        case PRESET_PID_STATE_RESET:
        case SEQ_PID_CLOCK_SRC:
        case SEQ_PID_SEQ_MODE:
        case SEQ_PID_SEQ_STEPS:
        case SEQ_PID_FILL:    /* momentary performance control */
        case SEQ_PID_POS:     /* firmware-written telemetry */
        case SEQ_PID_CURPAT:
        case SEQ_PID_REV:     /* pattern-data revision: a counter, not a setting */
        case SEQ_PID_PATTERN: /* navigation, owned by the sequence slots */
        case SEQ_PID_EDIT_TRACK:
        case SEQ_PID_EDIT_STEP:
            return true;
        default:
            break;
    }
#if SYNTH_ENABLE_MODULAR
    /* Graph telemetry (S28): both are firmware-written and derived from the
     * blob this file already stores. Saving them would be harmless; loading
     * them would not — a stale cost or revision written over the live ones
     * would make the app's budget meter and its "re-read the graph" trigger
     * disagree with the graph that is actually bound. */
    if (id == osynth::graph::PID_GRAPH_COST ||
        id == osynth::graph::PID_GRAPH_REV) {
        return true;
    }
#endif
    return false;
}

/* ---- working state: what it covers (S40) --------------------------------
 *
 * The patch rule above, widened by exactly three ids and fenced by persist.
 *
 * The widening is the whole difference between a *named* snapshot and "the
 * box as you left it". engine.type, drums.kit and seq.pattern are excluded
 * from presets because loading a preset called "Bell" must not silently swap
 * the kit or jump the sequencer to another pattern — a named slot that moves
 * things the player did not name is a surprise. Restoring the instrument to
 * the state it was switched off in is the opposite case: leaving those three
 * behind is what would be surprising.
 *
 * engine.type is not in the pair list even so — it lives in the header,
 * because it has to be read and acted on before any 0x02xx value can land
 * anywhere meaningful. Same ordering rule as the graph blob.
 *
 * The fence is persist_owns(): master volume, the line input, the output
 * level and the USB role are NVS settings with their own write policy, and
 * one setting owned by two files is a value that depends on which owner lost
 * the race. Asking persist rather than repeating its list means the fence
 * still holds the day someone adds a setting there. */
bool state_id(uint16_t id) {
    if (id == osynth::PID_ENGINE_TYPE) return false; /* in the header */
    /* chord.rev counts edits to the user set; the set itself travels as a
     * blob further down this file, exactly as the pattern data and the graph
     * do. Storing the counter as well would restore a revision that no longer
     * describes anything — the same reason seq.rev and graph.rev are excluded. */
    if (id == CHORD_PID_REV) return false;
    /* skip_id first because it is a switch and persist_owns() is a scan, and
     * this runs on every parameter write in the instrument. */
    if (skip_id(id)) {
        /* Chord mode is skipped by presets and kept by the working state: it
         * describes how the keyboard behaves, which is part of "the box as
         * you left it" and no part of any named patch. */
        if (id < CHORD_PID_FIRST || id > CHORD_PID_LAST) {
            switch (id) {
                case DRUM_PID_KIT:
                case SEQ_PID_PATTERN:
                    break;
                default:
                    return false;
            }
        }
    }
    return !persist_owns(id);
}

/* Whether a write to `id` means the working state has moved — which depends
 * on who wrote it.
 *
 * ParamOrigin::Internal is the firmware talking to itself, and almost all of
 * it is transient: playhead telemetry, and above all *parameter locks*, which
 * rewrite a patch value on every locked step and put it back when the track
 * stops. Counting those as edits would leave the state dirty for as long as a
 * p-locked pattern played, and the defer budget would then force a
 * multi-kilobyte write straight through the playback it exists to stay out
 * of. It would also store the lock's momentary value rather than the one the
 * player set.
 *
 * The two exceptions are the revision counters. Pattern data and the graph
 * are not parameters, so a counter is the only way either of them can say it
 * changed at all — and both are change-filtered, so a Euclidean fill or a
 * whole-graph load marks the state dirty once rather than once per step.
 *
 * Every other origin is a player action — the app, MIDI, the local UI, or a
 * preset load, which is included deliberately: a preset selected and then
 * powered off has to come back selected. Those two carry the values the file
 * keeps in its header rather than in its pair list, which is why they are
 * named here and excluded from state_id(). */
bool state_tracks(uint16_t id, ParamOrigin origin) {
    if (origin == ParamOrigin::Internal) {
        if (id == SEQ_PID_REV) return true;
#if SYNTH_ENABLE_MODULAR
        if (id == osynth::graph::PID_GRAPH_REV) return true;
#endif
        return false;
    }
    if (id == osynth::PID_ENGINE_TYPE || id == PRESET_PID_LOAD) return true;
    return state_id(id);
}

/* Any control task; must stay short. Marking is all it does — the tick in
 * preset_task decides when. */
void state_touch(uint16_t id, ParamOrigin origin) {
    if (!s_state_ready.load(std::memory_order_relaxed)) return;
    if (!state_tracks(id, origin)) return;
    s_state_dirty.store(true, std::memory_order_relaxed);
    s_state_seq.fetch_add(1, std::memory_order_relaxed);
}

/* FNV-1a over the blob as it is built, so the write can be skipped when
 * nothing actually moved. Not a checksum on disk — nothing verifies the file
 * with it; it only ever compares this run against the last one. */
void hash_feed(uint32_t& h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
}
constexpr uint32_t kHashSeed = 2166136261u;

/* Bookkeeping write; origin Preset keeps the listener from re-queuing. */
void reflect(uint16_t pid, int value) {
    ParamStore::instance().set(pid, (float)value, ParamOrigin::Preset);
}

/* Fills s_pairs from the factory table or the slot's file; returns the
 * pair count with the name copied out, or -1 (already logged). */
int fetch_snapshot(int engine, int slot, char name[PRESETS_NAME_MAX]) {
    /* Cleared for *every* path, not just the one that can fill it: a
     * factory slot loaded straight after a version-2 user slot would
     * otherwise inherit the previous file's graph and re-patch the synth
     * with a patch nobody selected. */
    s_graph_len = 0;
    s_legacy_fx = false;
    if (slot < kUserFirst) {
        const factory_preset_t* f = &g_factory_presets[engine][slot];
        strlcpy(name, f->name, PRESETS_NAME_MAX);
        int n = f->count < kMaxPairs ? f->count : kMaxPairs;
        if (n > 0) memcpy(s_pairs, f->pairs, (size_t)n * sizeof(preset_pair_t));
        return n;
    }
    if (!s_fs_ok) {
        ESP_LOGW(TAG, "load %s/%d: storage unavailable", engine_name(engine),
                 slot);
        return -1;
    }
    char path[40];
    preset_path(path, sizeof(path), engine, slot);
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "load %s/%d: slot is empty", engine_name(engine), slot);
        return -1;
    }
    PresetHdr h;
    if (fread(&h, sizeof(h), 1, fp) != 1 || h.magic != kPresetMagic ||
        !preset_version_known(h.version) || h.engine != engine) {
        fclose(fp);
        ESP_LOGW(TAG, "load %s/%d: bad file header, ignoring",
                 engine_name(engine), slot);
        return -1;
    }
    s_legacy_fx = preset_version_pre_fx_on(h.version);
    int n = h.count < kMaxPairs ? h.count : kMaxPairs;
    n = (int)fread(s_pairs, sizeof(preset_pair_t), (size_t)n, fp);
    /* A v2 file carries the graph after the pairs. Read it here rather than
     * in do_load so the file is opened once and closed once; do_load decides
     * what to do with it, because that is where the ordering against the
     * parameter snapshot is enforced. */
    s_graph_len = 0;
#if SYNTH_ENABLE_MODULAR
    if (preset_version_has_graph(h.version)) {
        s_graph_len = fread(s_graph_blob, 1, sizeof(s_graph_blob), fp);
    }
#endif
    fclose(fp);
    h.name[PRESETS_NAME_MAX - 1] = '\0';
    strlcpy(name, h.name, PRESETS_NAME_MAX);
    return n;
}

/* Defaults first, stored values on top — a sparse snapshot lands on a
 * defined state and files missing newer params stay valid. */
/* The eight units that gained a switch in S36, as {mix, on} id pairs.
 *
 * The three that already had one (filter, EQ, compressor) are deliberately
 * absent: their switch has always been the gate, so a legacy file already
 * carries the right value for it and inferring one would overwrite the
 * player's actual choice with a guess. */
struct FxOnPair {
    uint16_t mix;
    uint16_t on;
};
const FxOnPair kFxOnPairs[] = {
    {FX_PID_CHO_MIX, FX_PID_CHO_ON},     {FX_PID_DLY_MIX, FX_PID_DLY_ON},
    {FX_PID_GRN_MIX, FX_PID_GRN_ON},     {FX_PID_REV_MIX, FX_PID_REV_ON},
    {FX_PID_CRUSH_MIX, FX_PID_CRUSH_ON}, {FX_PID_DRV_MIX, FX_PID_DRV_ON},
    {FX_PID_PHS_MIX, FX_PID_PHS_ON},     {FX_PID_FLG_MIX, FX_PID_FLG_ON},
};

/* Turn on every effect a pre-S36 file had audible, so it sounds on load the
 * way it sounded when it was saved.
 *
 * Runs against the parameter store rather than against the pair list, and
 * that is the important part: a v1 file that never mentions fx.rev.mix still
 * gets the registered default of 0.15, which on pre-S36 firmware was an
 * audible reverb. Reading the store catches that; scanning the file's pairs
 * would not, and the patch would come back dry.
 *
 * Deliberately one-way. Nothing here can turn a switch *off*, so running it
 * on a file that does not need it would be harmless — it is gated on the
 * version anyway, because a v3 file's bypassed units are a decision and not
 * an absence. */
void legacy_fx_enable() {
    ParamStore& ps = ParamStore::instance();
    for (const FxOnPair& p : kFxOnPairs) {
        if (ps.describe(p.mix) == nullptr || ps.describe(p.on) == nullptr)
            continue;
        if (ps.get(p.mix) > 0.0f) ps.set(p.on, 1.0f, ParamOrigin::Preset);
    }
}

/* Rides the S6 switch protocol: request the engine, then wait for the switch
 * task to bind it — its 0x02xx parameters included — before anything is
 * applied on top. False when the switch did not complete (already logged);
 * `what` only labels the log line.
 *
 * Needs the audio task running. The detach handshake hands the voice pool
 * over on two render boundaries, so before audio_io_start() there is no
 * boundary to hand over on and the switch is refused — which is why the
 * working state's restore is queued from main() *after* the audio task and
 * not from presets_init(). */
bool switch_engine_wait(int engine, const char* what) {
    ParamStore& ps = ParamStore::instance();
    if ((int)engines_active_type() == engine) return true;
    ps.set(osynth::PID_ENGINE_TYPE, (float)engine, ParamOrigin::Preset);
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(kSwitchWaitMs);
    while ((int)engines_active_type() != engine) {
        /* the switch task reverts the param when a switch fails */
        if ((int)ps.get(osynth::PID_ENGINE_TYPE) != engine ||
            xTaskGetTickCount() > deadline) {
            ESP_LOGW(TAG, "%s %s: engine switch did not complete", what,
                     engine_name(engine));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

void apply_snapshot(int count, bool legacy_fx) {
    ParamStore& ps = ParamStore::instance();
    const size_t n = ps.listIds(s_ids, ParamStore::kMaxParams);
    for (size_t i = 0; i < n; ++i) {
        if (skip_id(s_ids[i])) continue;
        const ParamDesc* d = ps.describe(s_ids[i]);
        if (d != nullptr) ps.set(s_ids[i], d->def, ParamOrigin::Preset);
    }
    for (int i = 0; i < count; ++i) {
        if (skip_id(s_pairs[i].id)) continue;
        /* unregistered ids (older firmware, foreign engine) fail silently */
        ps.set(s_pairs[i].id, s_pairs[i].val, ParamOrigin::Preset);
    }
    /* After the pairs, never before: the inference reads the mix values the
     * file just set. */
    if (legacy_fx) legacy_fx_enable();
}

void do_load(int linear) {
    const int engine = linear / PRESETS_PER_ENGINE;
    const int slot = linear % PRESETS_PER_ENGINE;
    if (linear < 0 || engine >= SYNTH_ENGINE_COUNT) {
        reflect(PRESET_PID_LOAD, s_cur_load);
        return;
    }

    char name[PRESETS_NAME_MAX];
    const int count = fetch_snapshot(engine, slot, name);
    if (count < 0) {
        reflect(PRESET_PID_LOAD, s_cur_load);
        return;
    }

    if (!switch_engine_wait(engine, "load")) {
        reflect(PRESET_PID_LOAD, s_cur_load);
        return;
    }

#if SYNTH_ENABLE_MODULAR
    /* The graph goes in *before* the values, and the ordering is the whole
     * subtlety of loading a modular preset. A node parameter id only exists
     * while its slot holds a kind that defines it: apply the pairs first and
     * every id belonging to a slot the stored patch fills differently would
     * be dropped as unregistered — the patch would come back wired correctly
     * and set to defaults. Same shape as S27's rule that `seq.pattern` is
     * applied before the rest of a set. */
    if (engine == SYNTH_ENGINE_MODULAR && s_graph_len > 0) {
        osynth::graph::Model m;
        if (osynth::graph::deserialize(s_graph_blob, s_graph_len, m)) {
            const esp_err_t rc = osynth::graph::load_model(m);
            if (rc != ESP_OK) {
                /* Keep loading: the values still land on whatever graph is
                 * bound, which is a closer result than abandoning the load,
                 * and the log says exactly what was refused. */
                ESP_LOGW(TAG, "load %s/%d: graph rejected (%s), values only",
                         engine_name(engine), slot, esp_err_to_name(rc));
            }
        } else {
            ESP_LOGW(TAG, "load %s/%d: graph blob malformed, values only",
                     engine_name(engine), slot);
        }
    }
#endif

    apply_snapshot(count, s_legacy_fx);
    s_cur_load = linear;
    reflect(PRESET_PID_LOAD, linear);
    ESP_LOGI(TAG, "loaded %s/%d '%s' (%s, %d values)", engine_name(engine),
             slot, name, slot < kUserFirst ? "factory" : "user", count);
}

/* Temp-file + rename so an interrupted save never clobbers the old file. */
bool write_file(const char* tmp, const char* path, const void* hdr,
                size_t hdr_len, const void* body, size_t body_len,
                const void* tail = nullptr, size_t tail_len = 0) {
    FILE* fp = fopen(tmp, "wb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "cannot create %s", tmp);
        return false;
    }
    bool ok = fwrite(hdr, hdr_len, 1, fp) == 1;
    if (ok && body_len > 0) ok = fwrite(body, body_len, 1, fp) == 1;
    /* Optional third section, used by version-2 presets for the modular
     * graph blob (S28). Kept as a separate buffer rather than concatenated
     * by the caller because the pairs array is a fixed static and the blob
     * has nothing to do with it. */
    if (ok && tail_len > 0) ok = fwrite(tail, tail_len, 1, fp) == 1;
    fclose(fp);
    if (!ok || rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "write to %s failed (filesystem full?)", path);
        remove(tmp);
        return false;
    }
    return true;
}

void do_save(int linear, const char* name_in) {
    const int engine = linear / PRESETS_PER_ENGINE;
    const int slot = linear % PRESETS_PER_ENGINE;
    bool ok = false;
    do {
        if (linear < 0 || engine >= SYNTH_ENGINE_COUNT) break;
        if (slot < kUserFirst) {
            ESP_LOGW(TAG, "save %d: slots 0-%d of a bank are factory "
                     "(read-only)", linear, kUserFirst - 1);
            break;
        }
        if ((int)engines_active_type() != engine) {
            ESP_LOGW(TAG,
                     "save %s/%d: bank does not match the active engine (%s)",
                     engine_name(engine), slot,
                     engine_name((int)engines_active_type()));
            break;
        }
        if (!s_fs_ok) {
            ESP_LOGW(TAG, "save: storage unavailable");
            break;
        }

        /* sparse snapshot: defaults are implicit (apply resets first) */
        ParamStore& ps = ParamStore::instance();
        const size_t n = ps.listIds(s_ids, ParamStore::kMaxParams);
        int count = 0;
        for (size_t i = 0; i < n; ++i) {
            if (skip_id(s_ids[i])) continue;
            const ParamDesc* d = ps.describe(s_ids[i]);
            if (d == nullptr) continue;
            const float v = ps.get(s_ids[i]);
            if (v == d->def) continue;
            s_pairs[count].id = s_ids[i];
            s_pairs[count].val = v;
            ++count;
        }

        /* The graph is captured *before* the header is stamped, because
         * whether a blob exists is what decides the version. */
        size_t graph_len = 0;
#if SYNTH_ENABLE_MODULAR
        uint8_t graph_blob[osynth::graph::kSerialMaxBytes];
        if (engine == SYNTH_ENGINE_MODULAR) {
            graph_len = osynth::graph::serialize(osynth::graph::model(),
                                                 graph_blob, sizeof(graph_blob));
        }
#else
        const uint8_t* graph_blob = nullptr;
#endif

        PresetHdr h = {};
        h.magic = kPresetMagic;
        /* Always the current pair, never the legacy pair: a file this
         * firmware wrote has meaningful `on` values in it, including the
         * absent ones. */
        h.version = (graph_len > 0) ? kPresetVersionGraph : kPresetVersion;
        h.engine = (uint8_t)engine;
        h.count = (uint16_t)count;
        if (name_in != nullptr && name_in[0] != '\0') {
            strlcpy(h.name, name_in, sizeof(h.name));
        } else {
            snprintf(h.name, sizeof(h.name), "user %d", slot);
        }

        char tmp[40], path[40];
        snprintf(tmp, sizeof(tmp), "%s/tmp.osp", kBasePath);
        preset_path(path, sizeof(path), engine, slot);
        if (!write_file(tmp, path, &h, sizeof(h), s_pairs,
                        (size_t)count * sizeof(preset_pair_t), graph_blob,
                        graph_len)) {
            break;
        }
        cache_store(engine, slot, h.name); /* the listing is served from RAM */
        ESP_LOGI(TAG, "saved %s/%d '%s' (%d non-default values, %u B)",
                 engine_name(engine), slot, h.name, count,
                 (unsigned)(sizeof(h) + (size_t)count * sizeof(preset_pair_t)));
        ok = true;
    } while (false);

    if (ok) s_cur_save = linear;
    reflect(PRESET_PID_SAVE, s_cur_save);
}

/* Sequence slots hold a whole S23 pattern — every track, its configuration
 * and its parameter locks — not the S12 recorder's 32 notes. The blob is far
 * too big for the preset task's stack, so it is built in a scratch buffer
 * allocated for the duration of the operation. Files written before S23 are
 * still readable: seq_pattern_deserialize() recognises the old layout. */
void do_seq_save(int slot) {
    if (slot < 0 || slot >= PRESETS_SEQ_SLOTS) return;
    if (!s_fs_ok) {
        ESP_LOGW(TAG, "seq save: storage unavailable");
        return;
    }
    const size_t cap = seqarp_pattern_max_bytes();
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (buf == nullptr) {
        ESP_LOGW(TAG, "seq save: no memory for a %u B pattern", (unsigned)cap);
        return;
    }
    const int pattern = seqarp_edit_pattern();
    const size_t n = seqarp_pattern_export(pattern, buf, cap);
    if (n == 0) {
        ESP_LOGW(TAG, "seq save: pattern %d could not be serialised",
                 pattern + 1);
        free(buf);
        return;
    }

    char tmp[40], path[40];
    snprintf(tmp, sizeof(tmp), "%s/tmp.osq", kBasePath);
    seq_path(path, sizeof(path), slot);
    if (write_file(tmp, path, buf, n, nullptr, 0)) {
        ESP_LOGI(TAG, "sequence saved: slot %d = pattern %d (%u B)", slot,
                 pattern + 1, (unsigned)n);
    }
    free(buf);
}

void do_seq_load(int slot) {
    if (slot < 0 || slot >= PRESETS_SEQ_SLOTS) return;
    if (!s_fs_ok) {
        ESP_LOGW(TAG, "seq load: storage unavailable");
        return;
    }
    char path[40];
    seq_path(path, sizeof(path), slot);
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "seq load: slot %d is empty", slot);
        return;
    }
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    const size_t cap = seqarp_pattern_max_bytes();
    if (len <= 0 || (size_t)len > cap) {
        ESP_LOGW(TAG, "seq load: slot %d has an implausible size (%ld B)", slot,
                 len);
        fclose(fp);
        return;
    }
    uint8_t* buf = (uint8_t*)malloc((size_t)len);
    if (buf == nullptr) {
        ESP_LOGW(TAG, "seq load: no memory for %ld B", len);
        fclose(fp);
        return;
    }
    const bool read_ok = fread(buf, 1, (size_t)len, fp) == (size_t)len;
    fclose(fp);

    const int pattern = seqarp_edit_pattern();
    if (!read_ok || !seqarp_pattern_import(pattern, buf, (size_t)len)) {
        ESP_LOGW(TAG, "seq load: slot %d has a bad file, ignoring", slot);
    } else {
        ESP_LOGI(TAG, "sequence loaded: slot %d -> pattern %d (%ld B)", slot,
                 pattern + 1, len);
    }
    free(buf);
}

/* ---- whole-sequencer sets (S27) ----------------------------------------
 *
 * A set is the sequencer's side of a song: every pattern, the chain that
 * orders them, and the 0x04xx parameters that are part of the arrangement.
 * Rig setup and transport are excluded — the clock source belongs to the
 * studio, not to the music, and a load that started or stopped the transport
 * would be a trap. seq.scale/root/swing are excluded too: they mirror the
 * edited pattern's configuration, which travels inside its blob and is
 * republished by seqarp_pattern_reflect().
 */
bool seqset_id(uint16_t id) {
    if (id < osynth::PID_SEQARP_BASE || id >= osynth::PID_MODMATRIX_BASE) {
        return false;
    }
    /* Chord mode shares the 0x04xx namespace but not the reasoning: it is a
     * performance setting, and a set is the sequencer's side of a song.
     * Loading someone's arrangement should not reach across and change what
     * your keyboard plays. */
    if (id >= CHORD_PID_FIRST && id <= CHORD_PID_LAST) return false;
    switch (id) {
        case SEQ_PID_CLOCK_SRC:  /* rig setup */
        case SEQ_PID_SEQ_MODE:   /* transport */
        case SEQ_PID_SEQ_STEPS:  /* mirror of the edited track's length */
        case SEQ_PID_FILL:       /* momentary performance control */
        case SEQ_PID_POS:        /* firmware-written telemetry */
        case SEQ_PID_CURPAT:
        case SEQ_PID_REV:        /* pattern-data revision, not a setting */
        case SEQ_PID_EDIT_TRACK: /* editor cursors */
        case SEQ_PID_EDIT_STEP:
        case SEQ_PID_SCALE:      /* pattern data, restored with the pattern */
        case SEQ_PID_ROOT:
        case SEQ_PID_SWING:
            return false;
        default:
            return true;
    }
}

void do_set_save(int slot) {
    if (slot < 0 || slot >= PRESETS_SET_SLOTS) return;
    if (!s_fs_ok) {
        ESP_LOGW(TAG, "set save: storage unavailable");
        return;
    }
    const size_t cap = seqarp_pattern_max_bytes();
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (buf == nullptr) {
        ESP_LOGW(TAG, "set save: no memory for a %u B pattern", (unsigned)cap);
        return;
    }

    /* Every arrangement parameter, not just the non-default ones: a set is a
     * complete state, and a load that silently kept the current tempo because
     * the saved one happened to be 120 would be indefensible. */
    ParamStore& ps = ParamStore::instance();
    const size_t total_ids = ps.listIds(s_ids, ParamStore::kMaxParams);
    int params = 0;
    for (size_t i = 0; i < total_ids; ++i) {
        if (!seqset_id(s_ids[i])) continue;
        s_pairs[params].id = s_ids[i];
        s_pairs[params].val = ps.get(s_ids[i]);
        ++params;
    }

    seq_song_entry_t song[SEQ_SONG_MAX];
    int song_len = seq_song_length();
    if (song_len < 0) song_len = 0;
    if (song_len > SEQ_SONG_MAX) song_len = SEQ_SONG_MAX;
    for (int i = 0; i < song_len; ++i) seq_song_get(i, &song[i]);

    SetHdr h = {};
    h.magic = kSetMagic;
    h.version = kSetVersion;
    h.patterns = (uint8_t)SEQ_PATTERNS;
    h.song_len = (uint8_t)song_len;
    h.params = (uint16_t)params;
    snprintf(h.name, sizeof(h.name), "set %d", slot + 1);

    char tmp[40], path[48];
    snprintf(tmp, sizeof(tmp), "%s/tmp.oss", kBasePath);
    set_path(path, sizeof(path), slot);

    FILE* fp = fopen(tmp, "wb");
    bool ok = fp != nullptr;
    if (!ok) ESP_LOGW(TAG, "cannot create %s", tmp);
    if (ok) ok = fwrite(&h, sizeof(h), 1, fp) == 1;
    if (ok && params > 0) {
        ok = fwrite(s_pairs, sizeof(preset_pair_t), (size_t)params, fp) ==
             (size_t)params;
    }
    if (ok && song_len > 0) {
        ok = fwrite(song, sizeof(song[0]), (size_t)song_len, fp) ==
             (size_t)song_len;
    }
    size_t bytes = sizeof(h) + (size_t)params * sizeof(preset_pair_t) +
                   (size_t)song_len * sizeof(song[0]);
    for (int p = 0; ok && p < SEQ_PATTERNS; ++p) {
        const uint32_t len = (uint32_t)seqarp_pattern_export(p, buf, cap);
        if (len == 0) {
            ESP_LOGW(TAG, "set save: pattern %d could not be serialised",
                     p + 1);
            ok = false;
            break;
        }
        ok = fwrite(&len, sizeof(len), 1, fp) == 1 &&
             fwrite(buf, 1, len, fp) == len;
        bytes += sizeof(len) + len;
    }
    if (fp != nullptr) fclose(fp);
    free(buf);

    if (!ok || rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "set save: writing slot %d failed (filesystem full?)",
                 slot);
        remove(tmp);
        return;
    }
    ESP_LOGI(TAG,
             "set saved: slot %d = %d pattern(s), %d song step(s), %d "
             "param(s), %u B",
             slot, SEQ_PATTERNS, song_len, params, (unsigned)bytes);
    reflect(PRESET_PID_SEQSET_SAVE, slot);
}

void do_set_load(int slot) {
    if (slot < 0 || slot >= PRESETS_SET_SLOTS) return;
    if (!s_fs_ok) {
        ESP_LOGW(TAG, "set load: storage unavailable");
        return;
    }
    char path[48];
    set_path(path, sizeof(path), slot);
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "set load: slot %d is empty", slot);
        return;
    }

    SetHdr h;
    if (fread(&h, sizeof(h), 1, fp) != 1 || h.magic != kSetMagic ||
        h.version != kSetVersion || h.params > kMaxPairs ||
        h.song_len > SEQ_SONG_MAX) {
        fclose(fp);
        ESP_LOGW(TAG, "set load: slot %d has a bad file header, ignoring", slot);
        return;
    }
    h.name[PRESETS_NAME_MAX - 1] = '\0';

    const int params = (int)h.params;
    if (params > 0 &&
        (int)fread(s_pairs, sizeof(preset_pair_t), (size_t)params, fp) !=
            params) {
        fclose(fp);
        ESP_LOGW(TAG, "set load: slot %d is truncated", slot);
        return;
    }
    seq_song_entry_t song[SEQ_SONG_MAX];
    const int song_len = (int)h.song_len;
    if (song_len > 0 &&
        (int)fread(song, sizeof(song[0]), (size_t)song_len, fp) != song_len) {
        fclose(fp);
        ESP_LOGW(TAG, "set load: slot %d is truncated", slot);
        return;
    }

    const size_t cap = seqarp_pattern_max_bytes();
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (buf == nullptr) {
        fclose(fp);
        ESP_LOGW(TAG, "set load: no memory for a %u B pattern", (unsigned)cap);
        return;
    }

    int loaded = 0;
    for (int p = 0; p < (int)h.patterns; ++p) {
        uint32_t len = 0;
        if (fread(&len, sizeof(len), 1, fp) != 1) break;
        if (len == 0) break;
        if (len > cap) {
            /* A pattern from a build with more tracks or steps than this one
             * (an S3 file on a classic ESP32): skip it rather than abandoning
             * the whole set. */
            ESP_LOGW(TAG, "set load: pattern %d is %u B, too big for this "
                     "build — skipped", p + 1, (unsigned)len);
            if (p < SEQ_PATTERNS) seq_pattern_clear(p);
            if (fseek(fp, (long)len, SEEK_CUR) != 0) break;
            continue;
        }
        if (fread(buf, 1, len, fp) != len) break;
        if (p < SEQ_PATTERNS && seqarp_pattern_import(p, buf, len)) ++loaded;
    }
    free(buf);
    fclose(fp);

    /* A set is the whole sequencer, so patterns the file does not carry are
     * cleared: loading has to be repeatable, and leaving stale patterns in
     * place would mean the next save wrote a set nobody assembled. */
    for (int p = (int)h.patterns; p < SEQ_PATTERNS; ++p) seq_pattern_clear(p);

    if (song_len > 0) {
        seq_song_set_length(song_len);
        for (int i = 0; i < song_len; ++i) seq_song_set(i, &song[i]);
    }

    /* seq.pattern goes first: the clock task resolves scale/root against
     * whichever pattern is selected, so the selection has to be in place
     * before the reflection below — otherwise the loaded pattern's feel is
     * written into whatever pattern happened to be open. */
    ParamStore& ps = ParamStore::instance();
    for (int i = 0; i < params; ++i) {
        if (s_pairs[i].id == SEQ_PID_PATTERN) {
            ps.set(s_pairs[i].id, s_pairs[i].val, ParamOrigin::Preset);
        }
    }
    seqarp_pattern_reflect(seqarp_edit_pattern());
    for (int i = 0; i < params; ++i) {
        /* Anything else the file carries is ignored — the skip list is the
         * authority on what a set may touch, whatever wrote the file. */
        if (s_pairs[i].id == SEQ_PID_PATTERN || !seqset_id(s_pairs[i].id)) {
            continue;
        }
        ps.set(s_pairs[i].id, s_pairs[i].val, ParamOrigin::Preset);
    }

    ESP_LOGI(TAG, "set loaded: slot %d '%s' — %d pattern(s), %d song step(s), "
             "%d param(s)", slot, h.name[0] != '\0' ? h.name : "?", loaded,
             song_len, params);
    reflect(PRESET_PID_SEQSET_LOAD, slot);
}

/* ---- the working state (S40) --------------------------------------------
 *
 * One unnamed slot that saves itself. See presets.h for what it carries and
 * why, and state_id() above for the fence around it.
 */

void state_path(char* out, size_t n) {
    snprintf(out, n, "%s/state.osw", kBasePath);
}

/* Where a section of the blob goes. `fp == nullptr` is the probe pass: it
 * builds and hashes exactly the same bytes without touching flash, which is
 * what lets a save that would change nothing cost nothing. */
struct StateSink {
    FILE* fp;
    uint32_t hash;
    size_t bytes;
    bool ok;

    void put(const void* d, size_t n) {
        if (!ok || n == 0) return;
        hash_feed(hash, d, n);
        bytes += n;
        if (fp != nullptr && fwrite(d, 1, n, fp) != n) ok = false;
    }
};

/* Builds the whole blob into `sk`. `buf`/`cap` is the pattern scratch — one
 * pattern at a time, so the caller allocates seqarp_pattern_max_bytes() once
 * and both passes share it. False if a section could not be produced or
 * written. */
bool build_state(StateSink& sk, uint8_t* buf, size_t cap) {
    ParamStore& ps = ParamStore::instance();

    /* Sparse, exactly like a preset: defaults are implicit, because the load
     * resets before it applies. A firmware that adds a parameter therefore
     * boots it at its new default instead of at a value the old build never
     * had an opinion about. */
    const size_t total = ps.listIds(s_ids, ParamStore::kMaxParams);
    int count = 0;
    for (size_t i = 0; i < total; ++i) {
        if (!state_id(s_ids[i])) continue;
        const ParamDesc* d = ps.describe(s_ids[i]);
        if (d == nullptr) continue;
        const float v = ps.get(s_ids[i]);
        if (v == d->def) continue;
        s_pairs[count].id = s_ids[i];
        s_pairs[count].val = v;
        ++count;
    }

    size_t graph_len = 0;
#if SYNTH_ENABLE_MODULAR
    if ((int)engines_active_type() == SYNTH_ENGINE_MODULAR) {
        graph_len = osynth::graph::serialize(osynth::graph::model(),
                                             s_graph_blob,
                                             sizeof(s_graph_blob));
    }
#endif

    seq_song_entry_t song[SEQ_SONG_MAX];
    int song_len = seq_song_length();
    if (song_len < 0) song_len = 0;
    if (song_len > SEQ_SONG_MAX) song_len = SEQ_SONG_MAX;
    for (int i = 0; i < song_len; ++i) seq_song_get(i, &song[i]);

    uint8_t chord_set[CHORD_USER_SLOTS * sizeof(chord_user_slot_t)];
    const size_t chord_len = chord_user_export(chord_set, sizeof(chord_set));

    StateHdr h = {};
    h.magic = kStateMagic;
    h.version = kStateVersion;
    h.engine = (uint8_t)engines_active_type();
    h.patterns = (uint8_t)SEQ_PATTERNS;
    h.params = (uint16_t)count;
    h.graph_len = (uint16_t)graph_len;
    h.song_len = (uint8_t)song_len;
    h.chord_len = (uint8_t)chord_len;
    h.preset = (int16_t)s_cur_load;

    sk.put(&h, sizeof(h));
    if (count > 0) sk.put(s_pairs, (size_t)count * sizeof(preset_pair_t));
#if SYNTH_ENABLE_MODULAR
    if (graph_len > 0) sk.put(s_graph_blob, graph_len);
#endif
    if (song_len > 0) sk.put(song, (size_t)song_len * sizeof(song[0]));

    for (int p = 0; p < SEQ_PATTERNS; ++p) {
        const uint32_t len = (uint32_t)seqarp_pattern_export(p, buf, cap);
        if (len == 0) return false; /* a pattern that will not serialise */
        sk.put(&len, sizeof(len));
        sk.put(buf, len);
    }
    /* Last, and only because it is last can it be optional: a reader that
     * does not know about this section has already finished by the time it
     * would start. */
    if (chord_len > 0) sk.put(chord_set, chord_len);
    return sk.ok;
}

/* Records what the live state hashes to *right now*, so the next save can
 * tell whether anything actually moved. Called after a restore and after a
 * reset — both leave the synth in a state that is by definition already
 * stored (or, for a reset, deliberately not worth storing until it is
 * played). Costs no flash: the probe sink writes nowhere. */
void state_baseline(uint8_t* buf, size_t cap) {
    StateSink probe = {nullptr, kHashSeed, 0, true};
    if (build_state(probe, buf, cap)) {
        s_state_hash = probe.hash;
        s_state_hash_valid = true;
    } else {
        s_state_hash_valid = false;
    }
}

/* `why` only labels the log line. Preset task only — it owns s_pairs, s_ids
 * and the graph staging buffer, and this walks all three.
 *
 * False means "wanted to write and could not", which is what puts the dirty
 * flag back: losing an edit to a full filesystem and never trying again would
 * be the same silent data loss the whole file exists to prevent. "Nothing to
 * write" is true — there is nothing to retry. */
bool do_state_save(const char* why) {
    /* Never before the restore. A save that ran first would overwrite the
     * stored state with the defaults the synth boots at, which is the one
     * failure this whole mechanism must not have. */
    if (!s_state_ready.load(std::memory_order_relaxed) || !s_fs_ok) return true;

    const size_t cap = seqarp_pattern_max_bytes();
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (buf == nullptr) {
        ESP_LOGW(TAG, "state save: no memory for a %u B pattern",
                 (unsigned)cap);
        return false;
    }

    /* Probe first. The dirty flag only says a parameter was *written* — a
     * knob dragged back where it started, a preset re-loaded, a step toggled
     * twice — so without this pass an untouched session would still cost a
     * flash write every few minutes for the life of the instrument. */
    StateSink probe = {nullptr, kHashSeed, 0, true};
    if (!build_state(probe, buf, cap)) {
        ESP_LOGW(TAG, "state save: could not serialise the sequencer");
        free(buf);
        return false;
    }
    if (s_state_hash_valid && probe.hash == s_state_hash) {
        free(buf);
        return true; /* nothing actually moved */
    }

    char tmp[40], path[40];
    snprintf(tmp, sizeof(tmp), "%s/tmp.osw", kBasePath);
    state_path(path, sizeof(path));

    /* Temp file + rename, like every other write here: a power cut during the
     * save leaves the previous state intact rather than half of this one —
     * which matters more for this file than for any other, because it is the
     * one the synth reads on every boot. */
    FILE* fp = fopen(tmp, "wb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "cannot create %s", tmp);
        free(buf);
        return false;
    }
    StateSink w = {fp, kHashSeed, 0, true};
    const bool built = build_state(w, buf, cap);
    fclose(fp);
    free(buf);

    if (!built || !w.ok || rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "state save failed (filesystem full?)");
        remove(tmp);
        return false;
    }
    /* The hash of what was *written*, not of the probe: anything that moved
     * between the two passes is in the file and has to be in the baseline. */
    s_state_hash = w.hash;
    s_state_hash_valid = true;
    ESP_LOGI(TAG, "state saved: %u B [%s]", (unsigned)w.bytes, why);
    return true;
}

void do_state_load(void) {
    /* One scratch buffer for the whole thing: the sequencer sections are read
     * a pattern at a time, and the baseline pass at the end needs the same
     * space to serialise them back.
     *
     * Failing to get it is the one path that leaves s_state_ready false, and
     * deliberately: a build that cannot serialise a pattern cannot write the
     * file either, so arming the auto-save would only produce a warning every
     * few seconds for the life of the synth. */
    const size_t cap = seqarp_pattern_max_bytes();
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (buf == nullptr) {
        ESP_LOGW(TAG, "state: no memory for a %u B pattern — not restored, "
                 "and not saved either", (unsigned)cap);
        return;
    }

    /* Everything below either restores the file or leaves the defaults in
     * place; either way the baseline is taken from the live state on the way
     * out, so a restore that found nothing does not immediately write one. */

    do {
        if (!s_fs_ok) {
            ESP_LOGW(TAG, "state: storage unavailable — starting at defaults");
            break;
        }
        char path[40];
        state_path(path, sizeof(path));
        FILE* fp = fopen(path, "rb");
        if (fp == nullptr) {
            ESP_LOGI(TAG, "state: nothing stored yet — starting at defaults");
            break;
        }

        StateHdr h;
        if (fread(&h, sizeof(h), 1, fp) != 1 || h.magic != kStateMagic ||
            h.version != kStateVersion || h.params > kMaxPairs ||
            h.song_len > SEQ_SONG_MAX || h.engine >= SYNTH_ENGINE_COUNT ||
            h.graph_len > kGraphBlobMax) {
            fclose(fp);
            ESP_LOGW(TAG, "state: bad file header, ignoring");
            break;
        }

        const int params = (int)h.params;
        if (params > 0 &&
            (int)fread(s_pairs, sizeof(preset_pair_t), (size_t)params, fp) !=
                params) {
            fclose(fp);
            ESP_LOGW(TAG, "state: truncated, ignoring");
            break;
        }
        /* The blob is taken only when this build can hold it: a firmware
         * without the modular engine, or one with fewer node slots than the
         * firmware that wrote the file, steps over it and restores everything
         * else. Refusing the whole state because one section is for a bigger
         * build would throw away the patch and the sequencer with it — the
         * same rule the oversized-pattern branch below follows. */
        s_graph_len = 0;
        bool take_graph = false;
#if SYNTH_ENABLE_MODULAR
        take_graph = h.graph_len > 0 && h.graph_len <= sizeof(s_graph_blob);
#endif
        if (take_graph) {
#if SYNTH_ENABLE_MODULAR
            s_graph_len = fread(s_graph_blob, 1, h.graph_len, fp);
            if (s_graph_len != h.graph_len) {
                fclose(fp);
                ESP_LOGW(TAG, "state: truncated graph, ignoring");
                break;
            }
#endif
        } else if (h.graph_len > 0) {
            if (fseek(fp, (long)h.graph_len, SEEK_CUR) != 0) {
                fclose(fp);
                ESP_LOGW(TAG, "state: truncated graph, ignoring");
                break;
            }
        }
        seq_song_entry_t song[SEQ_SONG_MAX];
        const int song_len = (int)h.song_len;
        if (song_len > 0 &&
            (int)fread(song, sizeof(song[0]), (size_t)song_len, fp) !=
                song_len) {
            fclose(fp);
            ESP_LOGW(TAG, "state: truncated song, ignoring");
            break;
        }

        /* The engine before anything else: a 0x02xx id only means what the
         * bound engine says it means, so every value below would land on the
         * wrong control — or nowhere — until the right engine is in place.
         * Same ordering rule as the graph blob and as S27's seq.pattern. */
        const bool switched = switch_engine_wait((int)h.engine, "state");

#if SYNTH_ENABLE_MODULAR
        if (switched && (int)h.engine == SYNTH_ENGINE_MODULAR &&
            s_graph_len > 0) {
            osynth::graph::Model m;
            if (osynth::graph::deserialize(s_graph_blob, s_graph_len, m)) {
                const esp_err_t rc = osynth::graph::load_model(m);
                if (rc != ESP_OK) {
                    ESP_LOGW(TAG, "state: graph rejected (%s), values only",
                             esp_err_to_name(rc));
                }
            } else {
                ESP_LOGW(TAG, "state: graph blob malformed, values only");
            }
        }
#else
        (void)switched;
#endif

        /* Patterns, streamed straight out of the file into the model. Same
         * rule the set slots follow: the sequencer that comes back is the one
         * that was saved, so patterns the file does not carry are cleared
         * rather than left holding whatever was there before. */
        int loaded = 0;
        bool patterns_ok = true;
        for (int p = 0; p < (int)h.patterns; ++p) {
            uint32_t len = 0;
            if (fread(&len, sizeof(len), 1, fp) != 1 || len == 0) {
                patterns_ok = false;
                break;
            }
            if (len > cap) {
                /* A pattern from a build with more tracks or steps than this
                 * one — skip it rather than abandon the restore. */
                if (p < SEQ_PATTERNS) seq_pattern_clear(p);
                if (fseek(fp, (long)len, SEEK_CUR) != 0) {
                    patterns_ok = false;
                    break;
                }
                continue;
            }
            if (fread(buf, 1, len, fp) != len) {
                patterns_ok = false;
                break;
            }
            if (p < SEQ_PATTERNS && seqarp_pattern_import(p, buf, len)) {
                ++loaded;
            }
        }
        for (int p = (int)h.patterns; p < SEQ_PATTERNS; ++p) {
            seq_pattern_clear(p);
        }

        /* The user chord set (S41), which is positional — it starts wherever
         * the pattern blobs ended. `patterns_ok` is therefore load-bearing
         * rather than tidiness: a truncated pattern leaves the file position
         * somewhere arbitrary, and reading 96 bytes from there would import
         * whatever happened to be at that offset as a chord set. A set the
         * file does not carry keeps the defaults, which is what a pre-S41
         * file (chord_len 0) means too. */
        if (patterns_ok && h.chord_len > 0) {
            uint8_t chord_set[CHORD_USER_SLOTS * sizeof(chord_user_slot_t)];
            if (h.chord_len == sizeof(chord_set) &&
                fread(chord_set, 1, sizeof(chord_set), fp) ==
                    sizeof(chord_set)) {
                (void)chord_user_import(chord_set, sizeof(chord_set));
            } else {
                ESP_LOGW(TAG, "state: chord set is %u B, expected %u — kept "
                              "the defaults",
                         (unsigned)h.chord_len, (unsigned)sizeof(chord_set));
            }
        }
        fclose(fp);

        seq_song_set_length(song_len);
        for (int i = 0; i < song_len; ++i) seq_song_set(i, &song[i]);

        /* Defaults first, stored values on top — a sparse blob has to land on
         * a defined state, exactly as a preset does. */
        ParamStore& ps = ParamStore::instance();
        const size_t total = ps.listIds(s_ids, ParamStore::kMaxParams);
        for (size_t i = 0; i < total; ++i) {
            if (!state_id(s_ids[i])) continue;
            const ParamDesc* d = ps.describe(s_ids[i]);
            if (d != nullptr) ps.set(s_ids[i], d->def, ParamOrigin::Preset);
        }
        for (int i = 0; i < params; ++i) {
            /* The fence is the authority on what may be applied, whatever
             * wrote the file — a firmware that stops covering an id must not
             * have it pushed back in from an old state. */
            if (!state_id(s_pairs[i].id)) continue;
            ps.set(s_pairs[i].id, s_pairs[i].val, ParamOrigin::Preset);
        }

        /* Last, so the edited pattern's own scale/root/swing win over the
         * defaults the reset above wrote: they are pattern data, and the
         * pattern they belong to has only just been imported. */
        seqarp_pattern_reflect(seqarp_edit_pattern());

        /* Display only — reflect() writes with origin Preset, which the
         * listener ignores, so nothing is re-loaded. It just means the app's
         * "current preset" readout survives the power cycle with everything
         * else. */
        if (h.preset >= 0 && h.preset < kSlotCount) {
            s_cur_load = h.preset;
            reflect(PRESET_PID_LOAD, s_cur_load);
        }

        ESP_LOGI(TAG,
                 "state restored: %s, %d value(s), %d pattern(s), %d song "
                 "step(s), preset %d",
                 engine_name((int)h.engine), params, loaded, song_len,
                 (int)h.preset);
    } while (false);

    state_baseline(buf, cap);
    free(buf);
    s_state_dirty.store(false, std::memory_order_relaxed);
    s_state_ready.store(true, std::memory_order_relaxed);
}

/* Back to the state a first boot would have produced — before the working
 * state existed, that is simply what every boot produced.
 *
 * Deliberately *not* a factory reset. The NVS settings (master volume, the
 * line input, the output level, the USB role) survived a power cycle long
 * before this file did, and someone reaching for "reset the patch" is not
 * asking for their monitoring level back at 0.8 or their USB role flipped.
 * state_id() draws that line and this reuses it. The looper is left alone for
 * the same reason: it is not in the working state either, and silently wiping
 * a take someone is part-way through would be indefensible. */
void do_state_reset(void) {
    ParamStore& ps = ParamStore::instance();

    /* The engine first, for the ordering reason the restore documents: the
     * 0x02xx defaults that matter are the ones the *default* engine
     * registers. */
    const ParamDesc* ed = ps.describe(osynth::PID_ENGINE_TYPE);
    if (ed != nullptr) {
        (void)switch_engine_wait((int)ed->def, "reset");
    }

#if SYNTH_ENABLE_MODULAR
    osynth::graph::reset_model();
    if ((int)engines_active_type() == SYNTH_ENGINE_MODULAR) {
        /* Bound: the plan has to be recompiled from the model the reset just
         * wrote, or the audio task keeps rendering the old patch. */
        const osynth::graph::Model m = osynth::graph::model();
        (void)osynth::graph::load_model(m);
    }
#endif

    const size_t total = ps.listIds(s_ids, ParamStore::kMaxParams);
    for (size_t i = 0; i < total; ++i) {
        if (!state_id(s_ids[i])) continue;
        const ParamDesc* d = ps.describe(s_ids[i]);
        if (d != nullptr) ps.set(s_ids[i], d->def, ParamOrigin::Preset);
    }

    for (int p = 0; p < SEQ_PATTERNS; ++p) seq_pattern_clear(p);
    seq_song_set_length(0);
    seqarp_pattern_reflect(seqarp_edit_pattern());

    /* The user chord set is state the parameter loop above cannot reach — it
     * is a blob, like the patterns and the graph — so a reset that skipped it
     * would leave an edited set behind on an otherwise blank instrument. */
    chord_user_reset();

    s_cur_load = 0;
    reflect(PRESET_PID_LOAD, 0);

    if (s_fs_ok) {
        char path[40];
        state_path(path, sizeof(path));
        remove(path); /* absent is "never saved" as far as the next boot cares */
    }

    const size_t cap = seqarp_pattern_max_bytes();
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (buf != nullptr) {
        state_baseline(buf, cap);
        free(buf);
    } else {
        s_state_hash_valid = false;
    }
    s_state_dirty.store(false, std::memory_order_relaxed);

    /* Snaps back to 0 so the app can fire it again, and so nothing reads a
     * latched 1 as "still resetting". */
    reflect(PRESET_PID_STATE_RESET, 0);
    ESP_LOGI(TAG, "state reset to defaults");
}

/* Runs on the preset task between requests. The three gates are persist.h's,
 * for the same reason: a flash write parks the render chain, so it waits for
 * the edits to stop, then for the output to go quiet, and only gives up
 * waiting for silence after kStateMaxDeferMs — losing a session because
 * someone left a drone running is worse than one stall. */
void state_tick(void) {
    static uint32_t last_seq = 0;
    static uint32_t settled_ms = 0;
    static uint32_t waiting_ms = 0;

    if (!s_state_ready.load(std::memory_order_relaxed) ||
        !s_state_dirty.load(std::memory_order_relaxed)) {
        settled_ms = 0;
        waiting_ms = 0;
        return;
    }

    const uint32_t seq = s_state_seq.load(std::memory_order_relaxed);
    if (seq != last_seq) {
        last_seq = seq;
        settled_ms = 0; /* still moving — but the defer budget keeps running */
    } else {
        settled_ms += kStatePollMs;
    }
    waiting_ms += kStatePollMs;

    if (settled_ms < kStateSettleMs) return;

    const bool quiet = audio_io_quiet_ms() >= kStateQuietMs;
    const bool overdue = waiting_ms >= kStateMaxDeferMs;
    if (!quiet && !overdue) return;

    /* Cleared before the write, so a change that lands during it leaves the
     * state dirty again rather than being swallowed — and put back when the
     * write itself failed, so a transient full filesystem costs a retry rather
     * than the session. */
    s_state_dirty.store(false, std::memory_order_relaxed);
    if (!do_state_save(quiet ? "quiet" : "overdue")) {
        s_state_dirty.store(true, std::memory_order_relaxed);
    }
    settled_ms = 0;
    waiting_ms = 0;
}

/* Requests when there are any, the working-state tick when there are not.
 * The tick lives here rather than on a task of its own because everything it
 * can decide to do — serialising the sequencer, writing the file — has to
 * happen on this task anyway: it is the single consumer of s_pairs, s_ids and
 * the graph staging buffer, and that is what makes those statics safe. */
/* Hold the preset task until the output has been quiet for kSaveQuietMs, or
 * until kSaveWaitMs runs out. See the constants for why the budget is small.
 *
 * Only the write paths call this. A *load* reads flash through the cache and
 * costs nothing to speak of; it is erase and program that stop core 1. */
void wait_for_quiet(const char* what) {
    uint32_t waited = 0;
    while (audio_io_quiet_ms() < kSaveQuietMs) {
        if (waited >= kSaveWaitMs) {
            /* Not silent, and not silent about it: this is the line that
             * explains a click heard at the moment of saving. At INFO rather
             * than DEBUG because it is the whole point of the wait, and at
             * INFO rather than WARN because giving up after 400 ms is the
             * designed outcome and not a fault. One line per save at most —
             * the same task already logs the save itself. */
            ESP_LOGI(TAG, "%s: no quiet window in %u ms — writing anyway",
                     what, (unsigned)kSaveWaitMs);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(kSavePollMs));
        waited += kSavePollMs;
    }
}

void preset_task(void*) {
    Req r;
    for (;;) {
        if (xQueueReceive(s_queue, &r, pdMS_TO_TICKS(kStatePollMs)) != pdTRUE) {
            state_tick();
            continue;
        }
        switch (r.op) {
            case OP_LOAD:
                do_load(r.slot);
                break;
            case OP_SAVE:
                wait_for_quiet("preset save");
                do_save(r.slot, r.name[0] != '\0' ? r.name : nullptr);
                break;
            case OP_SEQ_LOAD:
                do_seq_load(r.slot);
                break;
            case OP_SEQ_SAVE:
                wait_for_quiet("pattern save");
                do_seq_save(r.slot);
                break;
            case OP_SET_LOAD:
                do_set_load(r.slot);
                break;
            case OP_SET_SAVE:
                wait_for_quiet("song save");
                do_set_save(r.slot);
                break;
            case OP_STATE_LOAD:
                do_state_load();
                break;
            case OP_STATE_SAVE:
                /* Deliberately not gated on quiet, unlike the three saves
                 * above. The routine path into this write is state_tick(),
                 * which already waits; the only other way in is
                 * presets_state_save_now(), whose caller is on its way to a
                 * reboot with a 5 s deadline. Spending part of that deadline
                 * listening for a gap would risk losing the session to save a
                 * glitch nobody is around to hear. */
                (void)do_state_save("forced");
                if (s_state_done != nullptr) xSemaphoreGive(s_state_done);
                break;
            case OP_STATE_RESET:
                do_state_reset();
                break;
            default:
                break;
        }
    }
}

bool queue_req(uint8_t op, int slot, const char* name) {
    if (s_queue == nullptr) return false;
    Req r = {};
    r.op = op;
    r.slot = (int16_t)slot;
    if (name != nullptr) strlcpy(r.name, name, sizeof(r.name));
    if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "request queue full, dropped op %u slot %d", op, slot);
        return false;
    }
    return true;
}

/* Any control task; must stay short — just queue. Origin Preset writes are
 * our own (apply/reflect) and never re-queue. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void*) {
    /* Before the origin gate, and deliberately: the working state follows
     * *every* origin, our own preset loads included. A preset loaded and then
     * powered off has to come back loaded, which is exactly the case a filter
     * on origin would drop. The restore's own writes are covered by
     * s_state_ready, which is false until it finishes. */
    state_touch(id, origin);
    if (origin == ParamOrigin::Preset) return;
    switch (id) {
        case PRESET_PID_LOAD:
            queue_req(OP_LOAD, (int)value, nullptr);
            break;
        case PRESET_PID_SAVE:
            queue_req(OP_SAVE, (int)value, nullptr);
            break;
        case PRESET_PID_SEQ_LOAD:
            queue_req(OP_SEQ_LOAD, (int)value, nullptr);
            break;
        case PRESET_PID_SEQ_SAVE:
            queue_req(OP_SEQ_SAVE, (int)value, nullptr);
            break;
        case PRESET_PID_SEQSET_LOAD:
            queue_req(OP_SET_LOAD, (int)value, nullptr);
            break;
        case PRESET_PID_SEQSET_SAVE:
            queue_req(OP_SET_SAVE, (int)value, nullptr);
            break;
        case PRESET_PID_STATE_RESET:
            /* Only the rising edge. The reset reflects the parameter back to
             * 0 when it is done, and that write must not queue a second one. */
            if (value > 0.0f) queue_req(OP_STATE_RESET, 0, nullptr);
            break;
        default:
            break;
    }
}

} // namespace

extern "C" esp_err_t presets_init(void) {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = kBasePath;
    conf.partition_label = kPartLabel;
    conf.format_if_mount_failed = true;
    const esp_err_t mnt = esp_vfs_littlefs_register(&conf);
    size_t total = 0, used = 0;
    if (mnt == ESP_OK) {
        s_fs_ok = true;
        esp_littlefs_info(kPartLabel, &total, &used);
        s_cache = (SlotCache*)calloc(kCacheEntries, sizeof(SlotCache));
        if (s_cache == nullptr) {
            ESP_LOGW(TAG, "no room for the %u B directory cache — listings "
                     "will read every slot",
                     (unsigned)(kCacheEntries * sizeof(SlotCache)));
        } else {
            cache_build();
        }
    } else {
        ESP_LOGW(TAG, "littlefs mount failed (%s) — factory presets only, "
                 "saving disabled", esp_err_to_name(mnt));
    }

    static const ParamDesc kParams[] = {
        {PRESET_PID_LOAD, "preset.load", ParamType::Int, ParamCurve::Linear,
         0.0f, (float)(kSlotCount - 1), 0.0f, nullptr, 0},
        {PRESET_PID_SAVE, "preset.save", ParamType::Int, ParamCurve::Linear,
         0.0f, (float)(kSlotCount - 1), (float)kUserFirst, nullptr, 0},
        {PRESET_PID_SEQ_LOAD, "preset.seq.load", ParamType::Int,
         ParamCurve::Linear, 0.0f, (float)(PRESETS_SEQ_SLOTS - 1), 0.0f,
         nullptr, 0},
        {PRESET_PID_SEQ_SAVE, "preset.seq.save", ParamType::Int,
         ParamCurve::Linear, 0.0f, (float)(PRESETS_SEQ_SLOTS - 1), 0.0f,
         nullptr, 0},
        {PRESET_PID_SEQSET_LOAD, "preset.seqset.load", ParamType::Int,
         ParamCurve::Linear, 0.0f, (float)(PRESETS_SET_SLOTS - 1), 0.0f,
         nullptr, 0},
        {PRESET_PID_SEQSET_SAVE, "preset.seqset.save", ParamType::Int,
         ParamCurve::Linear, 0.0f, (float)(PRESETS_SET_SLOTS - 1), 0.0f,
         nullptr, 0},
        /* A trigger, like the six above: write 1 and the synth goes back to
         * the state a first boot would have produced. It reflects itself back
         * to 0 when it is done. Excluded from every snapshot by skip_id() and
         * state_id() — a preset that fired a reset on load would be the worst
         * action parameter in the instrument. */
        {PRESET_PID_STATE_RESET, "state.reset", ParamType::Int,
         ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0},
    };
    ParamStore& ps = ParamStore::instance();
    if (ps.add(kParams, sizeof(kParams) / sizeof(kParams[0])) !=
        sizeof(kParams) / sizeof(kParams[0])) {
        return ESP_FAIL;
    }

    s_queue = xQueueCreate(8, sizeof(Req));
    if (s_queue == nullptr) return ESP_ERR_NO_MEM;
    s_state_done = xSemaphoreCreateBinary();
    if (s_state_done == nullptr) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(preset_task, "preset", kTaskStack, nullptr,
                                kTaskPrio, &s_task, 0) != pdPASS) {
        return ESP_FAIL;
    }
    if (ps.addListener(param_listener, nullptr) < 0) return ESP_FAIL;

    ESP_LOGI(TAG,
             "up: littlefs %u/%u KB used, %d factory + %d user presets x %d "
             "engines, %d seq slots, %d set slots, %d saved (cached) "
             "(preset.load/.save/.seq.*/.seqset.*)",
             (unsigned)(used / 1024), (unsigned)(total / 1024),
             PRESETS_FACTORY_SLOTS, PRESETS_PER_ENGINE - PRESETS_FACTORY_SLOTS,
             SYNTH_ENGINE_COUNT, PRESETS_SEQ_SLOTS, PRESETS_SET_SLOTS,
             s_cache_count);
    return ESP_OK;
}

extern "C" esp_err_t presets_request_load(int engine, int slot) {
    if (engine < 0 || engine >= SYNTH_ENGINE_COUNT || slot < 0 ||
        slot >= PRESETS_PER_ENGINE) {
        return ESP_ERR_INVALID_ARG;
    }
    return queue_req(OP_LOAD, engine * PRESETS_PER_ENGINE + slot, nullptr)
               ? ESP_OK
               : ESP_FAIL;
}

extern "C" esp_err_t presets_request_save(int engine, int slot,
                                          const char* name) {
    if (engine < 0 || engine >= SYNTH_ENGINE_COUNT || slot < kUserFirst ||
        slot >= PRESETS_PER_ENGINE) {
        return ESP_ERR_INVALID_ARG;
    }
    return queue_req(OP_SAVE, engine * PRESETS_PER_ENGINE + slot, name)
               ? ESP_OK
               : ESP_FAIL;
}

extern "C" esp_err_t presets_request_seq_load(int slot) {
    if (slot < 0 || slot >= PRESETS_SEQ_SLOTS) return ESP_ERR_INVALID_ARG;
    return queue_req(OP_SEQ_LOAD, slot, nullptr) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t presets_request_seq_save(int slot) {
    if (slot < 0 || slot >= PRESETS_SEQ_SLOTS) return ESP_ERR_INVALID_ARG;
    return queue_req(OP_SEQ_SAVE, slot, nullptr) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t presets_request_seqset_load(int slot) {
    if (slot < 0 || slot >= PRESETS_SET_SLOTS) return ESP_ERR_INVALID_ARG;
    return queue_req(OP_SET_LOAD, slot, nullptr) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t presets_state_restore(void) {
    return queue_req(OP_STATE_LOAD, 0, nullptr) ? ESP_OK : ESP_ERR_NO_MEM;
}

extern "C" esp_err_t presets_state_save_now(void) {
    /* Queued and then waited on, rather than written on the caller's task.
     * The write walks s_pairs, s_ids and the graph staging buffer, and those
     * are single-consumer statics owned by the preset task — writing them
     * from a second task to save one round trip would corrupt a preset load
     * that happened to be in flight.
     *
     * Not gated on the dirty flag: do_state_save() probes the content anyway
     * and returns without touching flash when nothing moved, which is the
     * same answer for less bookkeeping.
     *
     * The timeout is generous because the thing being waited for is a
     * filesystem write, and a caller that gave up early would go on to reboot
     * *during* it — which is precisely what the temp-file-and-rename exists to
     * survive, but there is no reason to arrange for it. */
    if (!s_state_ready.load(std::memory_order_relaxed)) return ESP_OK;
    if (s_state_done == nullptr) return ESP_ERR_INVALID_STATE;
    /* Drain any stale give, so the wait below is for *this* request. */
    xSemaphoreTake(s_state_done, 0);
    if (!queue_req(OP_STATE_SAVE, 0, nullptr)) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_state_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

extern "C" esp_err_t presets_request_seqset_save(int slot) {
    if (slot < 0 || slot >= PRESETS_SET_SLOTS) return ESP_ERR_INVALID_ARG;
    return queue_req(OP_SET_SAVE, slot, nullptr) ? ESP_OK : ESP_FAIL;
}

extern "C" bool presets_slot_info(int engine, int slot,
                                  char name[PRESETS_NAME_MAX], bool* factory) {
    if (engine < 0 || engine >= SYNTH_ENGINE_COUNT || slot < 0 ||
        slot >= PRESETS_PER_ENGINE) {
        return false;
    }
    if (factory != nullptr) *factory = slot < kUserFirst;
    if (slot < kUserFirst) {
        if (name != nullptr) {
            strlcpy(name, g_factory_presets[engine][slot].name,
                    PRESETS_NAME_MAX);
        }
        return true;
    }
    if (!s_fs_ok) return false;
    if (s_cache != nullptr) return cache_lookup(engine, slot, name);
    return read_slot_name(engine, slot, name); /* no cache: read the header */
}
