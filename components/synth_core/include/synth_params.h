/*
 * osynth — central parameter registry (ParamStore).
 *
 * Every adjustable value in the synthesizer is a registered parameter with a
 * stable 16-bit ID. All control surfaces (BLE, MIDI CC, presets, local UI)
 * and all consumers (engines, FX, sequencer) read/write through this store.
 * The BLE PARAM_INFO opcode serves ParamDesc metadata so the mobile app can
 * build its UI dynamically. See docs/PARAM_MAP.md for the ID map.
 *
 * Threading model:
 *  - set() / get() / valuePtr() are safe from any task: values are 32-bit
 *    atomics (lock-free on Xtensa).
 *  - The audio task must not call set()/add()/removeRange(); it caches
 *    valuePtr() pointers and reads them per block.
 *  - Listeners run synchronously on the task that called set(); keep them
 *    short and non-blocking.
 *  - add()/removeRange()/resetRange() are control-task only. Callers must
 *    guarantee the audio task no longer dereferences valuePtr()s of a range
 *    being removed (the engine-switch protocol mutes and detaches the old
 *    engine first).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace osynth {

/* Parameter ID namespaces — see docs/PARAM_MAP.md */
constexpr uint16_t PID_GLOBAL_BASE        = 0x0000; /* master volume, engine select… */
constexpr uint16_t PID_ENGINE_COMMON_BASE = 0x0100; /* glide, bend range, unison… */
constexpr uint16_t PID_ENGINE_BASE        = 0x0200; /* meaning depends on active engine */
constexpr uint16_t PID_FX_BASE            = 0x0300; /* chorus/delay/granular/reverb */
constexpr uint16_t PID_SEQARP_BASE        = 0x0400; /* sequencer + arpeggiator */
constexpr uint16_t PID_MODMATRIX_BASE     = 0x0500; /* 8 slots x source/dest/amount */
constexpr uint16_t PID_LOOPER_BASE        = 0x0600; /* 8-track loop recorder (S15) */
constexpr uint16_t PID_DRUMS_BASE         = 0x0700; /* drum bus + per-slot mixer (S22) */
constexpr uint16_t PID_SPACE_END          = 0x0800;

/* Global parameters (0x00xx) */
constexpr uint16_t PID_MASTER_VOLUME = 0x0000;
constexpr uint16_t PID_ENGINE_TYPE   = 0x0001;
/* 0x0002-0x0007 belong to the preset triggers (PRESET_PID_* in presets.h). */
/* Line input (S31), registered only on builds with SYNTH_ENABLE_LINE_IN.
 * The ids exist unconditionally so presets' skip_id() needs no #if. */
constexpr uint16_t PID_LINE_IN_ROUTE = 0x0008;
constexpr uint16_t PID_LINE_IN_GAIN  = 0x0009;
/* Analogue input gain, registered only where the hardware has one to set
 * (the ES8388's PGA). Deliberately separate from PID_LINE_IN_GAIN, which is
 * the digital trim and means exactly the same thing on every front end. */
constexpr uint16_t PID_LINE_IN_PGA   = 0x000A;
/* Analogue output driver level, likewise registered only where there is one.
 * Sets the operating point for what is plugged in — line level, or turned
 * down for headphones; master.volume stays the digital control the player
 * actually rides. */
constexpr uint16_t PID_OUT_LEVEL     = 0x000B;
/* Which role the USB-OTG port takes at boot (S35): 0 = device (audio + MIDI
 * to a computer), 1 = host (drives a USB MIDI controller). Registered only on
 * builds where both roles are reachable — see SYNTH_ENABLE_USB_HOST — so on a
 * build where USB is the audio clock the app never offers a control that
 * would silence the synth. Persisted, and read back at the next boot: the two
 * roles cannot share the one port, so a change needs a restart. */
constexpr uint16_t PID_USB_MODE      = 0x000C;
/* Which device feeds the one input chain (S37): 0 = line, 1 = mic. Registered
 * only on a build that has both compiled in — see SYNTH_ENABLE_IN_SOURCE_SEL.
 *
 * Persisted alongside the route and the trim, and for the same reason: it
 * describes what is plugged into the box, not what patch is loaded. Presets
 * skip it, so its default is what every existing patch keeps meaning — and
 * that default is `line`, which is the source every build before this one
 * had. */
constexpr uint16_t PID_LINE_IN_SOURCE = 0x000D;
/* Level trim for the microphone alone (S37b), so `both` is usable: a line
 * source arrives at whatever the ADC was set up to take, a MEMS mic at
 * conversational distance arrives far below full scale, and one shared
 * `in.gain` across the pair is a control that is wrong for one of them
 * whichever way it is set. Registered on the same condition as the selector,
 * since with one device `in.gain` already is the trim. */
constexpr uint16_t PID_LINE_IN_MICGAIN = 0x000E;

/* Engine-common parameters (0x01xx) — registered by the voice manager,
 * meaningful for every engine. C code uses the SYNTH_PID_* mirrors in
 * synth_params_c.h (parity checked by static_asserts in synth_params.cpp). */
constexpr uint16_t PID_COMMON_GLIDE      = 0x0100;
constexpr uint16_t PID_COMMON_BEND_RANGE = 0x0101;
constexpr uint16_t PID_COMMON_UNISON     = 0x0102;
constexpr uint16_t PID_COMMON_UNI_DETUNE = 0x0103;
constexpr uint16_t PID_COMMON_UNI_SPREAD = 0x0104;

enum class ParamType : uint8_t { Float, Int, Enum, Bool };

/* UI mapping hint, exported over BLE PARAM_INFO (e.g. cutoff wants Exp). */
enum class ParamCurve : uint8_t { Linear, Exp, Log };

/* Who changed a parameter — lets listeners avoid echoing back to the origin. */
enum class ParamOrigin : uint8_t { Internal, Ble, Midi, LocalUi, Preset };

struct ParamDesc {
    uint16_t id;
    const char* name;              /* stable, lowercase, dot-separated, <= 24 chars */
    ParamType type;
    ParamCurve curve;
    float min;
    float max;
    float def;
    const char* const* enum_names; /* ParamType::Enum only, else nullptr */
    uint8_t enum_count;
};

using ParamListener = void (*)(uint16_t id, float value, ParamOrigin origin, void* ctx);

class ParamStore {
public:
    /* Peak *concurrent* registration, not the size of the id space. The
     * 0x02xx range holds one engine at a time, so the bound engine's set is
     * what counts — and since S28 that peak is the modular graph rather than
     * a fixed engine: 12 slots x up to 6 parameters is 73 ids where the
     * widest fixed engine registers 30. With the drum bus (71), the
     * sequencer, the FX bus, the matrix and the looper all resident, a full
     * graph lands near 270 — 323 since S34, which added 53 to the FX bus
     * (six units, note-division sync and two LFOs), 326 since S35 added
     * the three FX level-compensation switches, and 340 since S36 added the
     * eight per-effect enable switches plus the reverb's algorithm selector
     * and its five shared stages, 354 since S38 added the vocoder's
     * fourteen, and 373 since S39 added the nineteen belonging to the two
     * noise-reduction units — 375 since S39b gave each of those a source
     * selector. Raised from 384 to 448 in
     * S34 to keep the margin that number was chosen for, because
     * overflow is per-parameter and partial — a patch would come up with some
     * of a node's controls missing rather than failing outright, which is a
     * far more confusing thing to debug than a refusal. */
    static constexpr size_t kMaxParams = 448;
    /* One per subscribing component: engines, presets, looper, drums,
     * ble_ctrl, persist, and codec on an ES8388 build — 7 as of S31b.
     * Raised from 8 to leave headroom,
     * because overflowing it is silent at the call site (addListener returns
     * -1) and a component that treats that as fatal turns it into a
     * bootloop. Four spare slots cost 32 bytes. */
    static constexpr size_t kMaxListeners = 12;

    static ParamStore& instance();

    /* Registers a parameter and initializes it to its default.
     * Fails (false) on: id out of range, id already registered, store full. */
    bool add(const ParamDesc& desc);

    /* Registers a batch; returns how many were added. */
    size_t add(const ParamDesc* descs, size_t count);

    /* Unregisters every parameter with first <= id < last_exclusive; returns
     * how many were removed. Used when switching engines (0x02xx range). */
    size_t removeRange(uint16_t first, uint16_t last_exclusive);

    /* Clamps to [min,max], snaps non-Float types to integers, stores, then
     * notifies listeners. Returns false if the id is not registered. */
    bool set(uint16_t id, float value, ParamOrigin origin = ParamOrigin::Internal);

    /* Returns the current value, or 0.0f if not registered. */
    float get(uint16_t id) const;

    /* Direct pointer to the atomic value for per-block reads on the audio
     * task. Returns nullptr if not registered. The pointer stays valid for
     * the lifetime of the store, but its slot may be reused after
     * removeRange() — see threading notes above. */
    const std::atomic<float>* valuePtr(uint16_t id) const;

    const ParamDesc* describe(uint16_t id) const;

    size_t count() const;

    /* Fills `out` with up to `max` registered IDs in ascending order;
     * returns the number written. */
    size_t listIds(uint16_t* out, size_t max) const;

    /* Resets every registered parameter in the range to its default. */
    void resetRange(uint16_t first, uint16_t last_exclusive);

    /* Bumped by every add() and every removeRange() that removed something,
     * i.e. once per engine bind and once per engine unbind — never while an
     * engine is simply playing.
     *
     * Exists because an id does not identify a parameter across a switch: the
     * 0x02xx range is re-registered per engine, so a holder that stashed
     * `{id, value}` (the sequencer's parameter locks) and wrote it back later
     * would land on whatever now owns that id. Capture this alongside the id
     * and skip the write when it no longer matches. Readable from any task. */
    uint32_t generation() const;

    /* Returns a handle (>= 0) or -1 if the listener table is full. */
    int addListener(ParamListener fn, void* ctx);
    void removeListener(int handle);

    /* Logs the whole registry (id, name, value, range) via ESP_LOGI. */
    void dump() const;

private:
    ParamStore();
    ParamStore(const ParamStore&) = delete;
    ParamStore& operator=(const ParamStore&) = delete;

    struct Entry {
        ParamDesc desc{};
        std::atomic<float> value{0.0f};
        bool used = false;
    };

    struct ListenerSlot {
        ParamListener fn = nullptr;
        void* ctx = nullptr;
    };

    Entry* entryFor(uint16_t id);
    const Entry* entryFor(uint16_t id) const;
    void notify(uint16_t id, float value, ParamOrigin origin);

    Entry entries_[kMaxParams];
    int16_t index_[PID_SPACE_END]; /* id -> slot in entries_, -1 if none */
    ListenerSlot listeners_[kMaxListeners];
    size_t count_ = 0;
    std::atomic<uint32_t> generation_{0};
};

} // namespace osynth
