#!/usr/bin/env python3
"""S36: teach presets.cpp about the per-effect enable switches.

Three changes, all in components/presets/presets.cpp:

 1. Two new on-disk versions (3 = pairs only, 4 = pairs + modular graph).
    Needed because a .osp is a SPARSE snapshot — do_save() omits anything
    equal to its default — so with fx.<unit>.on defaulting to 0, "no `on`
    pair in the file" cannot otherwise be told apart from "the player
    deliberately bypassed this unit". The version byte is what separates a
    file written before the switches existed from one written after.

 2. legacy_fx_enable(), run after a v1/v2 file's pairs are applied: every
    unit whose mix ended up above zero gets its switch turned on, because
    on the firmware that wrote the file that unit WAS audible. This is the
    "scan existing user patches" half of the feature — it happens on load,
    so nothing has to walk the filesystem and rewrite files, and a slot is
    migrated permanently the next time the player saves it.

 3. read_slot_name() accepts every known version instead of only v1. That
    is a pre-existing bug: do_save() has written v2 for the modular engine
    since S28, so modular user presets loaded fine but vanished from the
    slot listing after a reboot. Adding versions 3 and 4 would have made
    the same bug swallow every S36 preset, so it is fixed here.

Run from the repo root:  python tools/s36_patch_preset_migration.py
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "components" / "presets" / "presets.cpp"

OLD_VER = """/* version 1: header + `count` {id, value} pairs.
 * version 2 (S28): the same, then a modular graph blob — the node kinds and
 * cables that give the 0x02xx ids in the pairs their meaning. Written only
 * for the modular engine, so every other bank's files stay byte-identical
 * v1 and nothing already on the device is rewritten or invalidated. */"""

NEW_VER = """/* version 1: header + `count` {id, value} pairs.
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
 * now answers, and legacy_fx_enable() is what acts on the answer. */"""

OLD_CONST = """constexpr uint8_t kPresetVersion = 1;
constexpr uint8_t kPresetVersionGraph = 2;"""

NEW_CONST = """constexpr uint8_t kPresetVersionLegacy = 1;
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
}"""

OLD_READ_NAME = """    const bool ok = fread(&h, sizeof(h), 1, fp) == 1 &&
                    h.magic == kPresetMagic && h.version == 1;"""

NEW_READ_NAME = """    /* Every known version, not just v1. Hard-coding v1 here was a bug from
     * S28 onwards: do_save() writes v2 for the modular engine, so a modular
     * user preset loaded fine when asked for by number but disappeared from
     * the listing on the next boot, which reads as "my preset is gone". */
    const bool ok = fread(&h, sizeof(h), 1, fp) == 1 &&
                    h.magic == kPresetMagic && preset_version_known(h.version);"""

OLD_HDR_CHECK = """    if (fread(&h, sizeof(h), 1, fp) != 1 || h.magic != kPresetMagic ||
        (h.version != kPresetVersion && h.version != kPresetVersionGraph) ||
        h.engine != engine) {"""

NEW_HDR_CHECK = """    if (fread(&h, sizeof(h), 1, fp) != 1 || h.magic != kPresetMagic ||
        !preset_version_known(h.version) || h.engine != engine) {"""

OLD_GRAPH_READ = """    if (h.version == kPresetVersionGraph) {"""
NEW_GRAPH_READ = """    if (preset_version_has_graph(h.version)) {"""

OLD_STAMP = """        h.version = (graph_len > 0) ? kPresetVersionGraph : kPresetVersion;"""
NEW_STAMP = """        /* Always the current pair, never the legacy pair: a file this
         * firmware wrote has meaningful `on` values in it, including the
         * absent ones. */
        h.version = (graph_len > 0) ? kPresetVersionGraph : kPresetVersion;"""

OLD_APPLY = """void apply_snapshot(int count) {
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
}"""

NEW_APPLY = """/* The eight units that gained a switch in S36, as {mix, on} id pairs.
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
}"""


def main() -> int:
    src = SRC.read_text(encoding="utf-8")
    if "legacy_fx_enable" in src:
        print("nothing to do: presets.cpp already carries the S36 migration")
        return 0

    edits = [
        (OLD_VER, NEW_VER),
        (OLD_CONST, NEW_CONST),
        (OLD_READ_NAME, NEW_READ_NAME),
        (OLD_HDR_CHECK, NEW_HDR_CHECK),
        (OLD_GRAPH_READ, NEW_GRAPH_READ),
        (OLD_STAMP, NEW_STAMP),
        (OLD_APPLY, NEW_APPLY),
    ]
    for old, new in edits:
        if src.count(old) != 1:
            print(f"anchor matched {src.count(old)}x, expected 1:\n{old[:90]}")
            return 1
        src = src.replace(old, new)

    # fx.h for the FX_PID_* ids the migration table names.
    inc = '#include "engines.h"\n'
    if src.count(inc) != 1:
        print("could not place the fx.h include")
        return 1
    src = src.replace(inc, inc + '#include "fx.h" /* FX_PID_*: the S36 enable-switch migration */\n')

    SRC.write_text(src, encoding="utf-8")
    print("presets.cpp: versions 3/4, legacy_fx_enable(), listing fix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
