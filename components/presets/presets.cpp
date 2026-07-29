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

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "drums.h"
#include "engines.h"
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
 * v1 and nothing already on the device is rewritten or invalidated. */
struct __attribute__((packed)) PresetHdr {
    uint32_t magic;
    uint8_t version; /* 1, or 2 when a graph blob follows the pairs */
    uint8_t engine;
    uint16_t count;
    char name[PRESETS_NAME_MAX]; /* NUL-padded */
};
static_assert(sizeof(PresetHdr) == 32, "on-disk layout");
constexpr uint8_t kPresetVersion = 1;
constexpr uint8_t kPresetVersionGraph = 2;

#if SYNTH_ENABLE_MODULAR
/* Staging for the graph blob between fetch_snapshot() and do_load(). Both
 * run on the `preset` task, so a static is safe and saves ~130 bytes of
 * stack on a task that already carries the pairs array. */
uint8_t s_graph_blob[osynth::graph::kSerialMaxBytes];
#endif
size_t s_graph_len = 0;

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

enum Op : uint8_t {
    OP_LOAD, OP_SAVE, OP_SEQ_LOAD, OP_SEQ_SAVE, OP_SET_LOAD, OP_SET_SAVE
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

void preset_path(char* out, size_t n, int engine, int slot) {
    snprintf(out, n, "%s/p%d_%02d.osp", kBasePath, engine, slot);
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
    const bool ok = fread(&h, sizeof(h), 1, fp) == 1 &&
                    h.magic == kPresetMagic && h.version == 1;
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
    /* Drums (0x07xx): the per-slot mixer *is* part of the sound and is
     * stored, but the kit selection and the audition trigger are not — a
     * preset that silently swapped the kit (or fired a hit) on load would be
     * a surprise, and the kit list differs per device anyway. */
    switch (id) {
        case DRUM_PID_KIT:
        case DRUM_PID_TRIG:
        case osynth::PID_MASTER_VOLUME:
        case osynth::PID_ENGINE_TYPE:
        case PRESET_PID_LOAD:
        case PRESET_PID_SAVE:
        case PRESET_PID_SEQ_LOAD:
        case PRESET_PID_SEQ_SAVE:
        case PRESET_PID_SEQSET_LOAD:
        case PRESET_PID_SEQSET_SAVE:
        case SEQ_PID_CLOCK_SRC:
        case SEQ_PID_SEQ_MODE:
        case SEQ_PID_SEQ_STEPS:
        case SEQ_PID_FILL:    /* momentary performance control */
        case SEQ_PID_POS:     /* firmware-written telemetry */
        case SEQ_PID_CURPAT:
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
        (h.version != kPresetVersion && h.version != kPresetVersionGraph) ||
        h.engine != engine) {
        fclose(fp);
        ESP_LOGW(TAG, "load %s/%d: bad file header, ignoring",
                 engine_name(engine), slot);
        return -1;
    }
    int n = h.count < kMaxPairs ? h.count : kMaxPairs;
    n = (int)fread(s_pairs, sizeof(preset_pair_t), (size_t)n, fp);
    /* A v2 file carries the graph after the pairs. Read it here rather than
     * in do_load so the file is opened once and closed once; do_load decides
     * what to do with it, because that is where the ordering against the
     * parameter snapshot is enforced. */
    s_graph_len = 0;
#if SYNTH_ENABLE_MODULAR
    if (h.version == kPresetVersionGraph) {
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
void apply_snapshot(int count) {
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

    ParamStore& ps = ParamStore::instance();
    if ((int)engines_active_type() != engine) {
        /* ride the S6 switch protocol: request, then wait for the switch
         * task to bind the target engine (its 0x02xx params included) */
        ps.set(osynth::PID_ENGINE_TYPE, (float)engine, ParamOrigin::Preset);
        const TickType_t deadline =
            xTaskGetTickCount() + pdMS_TO_TICKS(kSwitchWaitMs);
        while ((int)engines_active_type() != engine) {
            /* the switch task reverts the param when a switch fails */
            if ((int)ps.get(osynth::PID_ENGINE_TYPE) != engine ||
                xTaskGetTickCount() > deadline) {
                ESP_LOGW(TAG, "load %s/%d: engine switch did not complete",
                         engine_name(engine), slot);
                reflect(PRESET_PID_LOAD, s_cur_load);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
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

    apply_snapshot(count);
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
    switch (id) {
        case SEQ_PID_CLOCK_SRC:  /* rig setup */
        case SEQ_PID_SEQ_MODE:   /* transport */
        case SEQ_PID_SEQ_STEPS:  /* mirror of the edited track's length */
        case SEQ_PID_FILL:       /* momentary performance control */
        case SEQ_PID_POS:        /* firmware-written telemetry */
        case SEQ_PID_CURPAT:
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

void preset_task(void*) {
    Req r;
    for (;;) {
        if (xQueueReceive(s_queue, &r, portMAX_DELAY) != pdTRUE) continue;
        switch (r.op) {
            case OP_LOAD:
                do_load(r.slot);
                break;
            case OP_SAVE:
                do_save(r.slot, r.name[0] != '\0' ? r.name : nullptr);
                break;
            case OP_SEQ_LOAD:
                do_seq_load(r.slot);
                break;
            case OP_SEQ_SAVE:
                do_seq_save(r.slot);
                break;
            case OP_SET_LOAD:
                do_set_load(r.slot);
                break;
            case OP_SET_SAVE:
                do_set_save(r.slot);
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
    };
    ParamStore& ps = ParamStore::instance();
    if (ps.add(kParams, sizeof(kParams) / sizeof(kParams[0])) !=
        sizeof(kParams) / sizeof(kParams[0])) {
        return ESP_FAIL;
    }

    s_queue = xQueueCreate(8, sizeof(Req));
    if (s_queue == nullptr) return ESP_ERR_NO_MEM;
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
