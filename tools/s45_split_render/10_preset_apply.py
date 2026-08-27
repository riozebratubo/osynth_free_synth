"""S45b: stop a preset load from setting ~270 parameters to trigger ~20 changes.

apply_snapshot() reset every registered parameter to its default and then
applied the file's stored values on top. That is correct -- presets save
sparsely, so the defaults pass is what an omitted parameter means -- but it
costs two things it does not need to:

  * every parameter the file *does* name is set twice, travelling through its
    default on the way to its value, and
  * every parameter already sitting at its default is set again for nothing.

Neither would matter if set() were just a store. It is not: notify() runs nine
listeners synchronously, and the ones that matter belong to the audio task. The
FX bus, the vocoder and the adaptive NR detect a structural change by comparing
the live parameter against a cached copy and rebuild *inside the render block
that notices* (fx.cpp: voc_rebuild, anr_rebuild, rev_collect_lines). A
redundant set is therefore a filter bank recomputed inside one 1.33 ms block --
which, on a voice stage already running at 65-98% of its budget, is an
underrun. A stored parameter driven through its default first can pay it twice,
once for a configuration nobody asked for.

So: mark what the file names, default only what it does not, and in both passes
set only where the value would actually change. The resulting state is
identical, in the same order, from far fewer notifications.

Deliberately local to apply_snapshot() rather than folded into
ParamStore::set(). A global "skip the notify when the value is unchanged" would
also silence reflect(), which re-sets a parameter to a value the app may
already believe in precisely so the app is told; that echo failing is a control
that sticks in the wrong position.

Run from the repo root: python tools/s45_split_render/10_preset_apply.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _edit import Editor

ed = Editor("components/presets/presets.cpp", skip_if="set_if_changed")

ed.sub(
    """void apply_snapshot(int count, bool legacy_fx) {
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
}""",
    """/* One bit per parameter id: which ones this file actually names (S45b).
 * 256 bytes, static rather than on the preset task's stack, and cleared at the
 * top of every apply. */
uint32_t s_named[osynth::PID_SPACE_END / 32];

inline bool id_named(uint16_t id) {
    return id < osynth::PID_SPACE_END &&
           (s_named[id >> 5] & (1u << (id & 31))) != 0;
}

/* set() only where the value would actually change.
 *
 * The saving is not the store — that is one relaxed atomic — but the notify()
 * behind it. Nine listeners run synchronously on this task for every set, and
 * the expensive ones do their work on the *audio* task: the FX bus, the
 * vocoder and the adaptive NR each detect a structural change by comparing the
 * live parameter against a cached copy, and rebuild inside the render block
 * that notices (fx.cpp — voc_rebuild(), anr_rebuild(), rev_collect_lines()).
 * A redundant set is not merely wasted work, it is a filter bank recomputed
 * inside one block period, which is exactly what an underrun is made of.
 *
 * Compares the raw value rather than the clamped one on purpose. set() clamps
 * and rounds, so a file holding a value a hair outside the range compares
 * unequal here and gets applied — one redundant set on a malformed file,
 * against having to keep a second copy of the clamp in step with the first.
 * An unregistered id reads 0 and either matches (nothing to apply, and set()
 * would have refused it anyway) or falls through to a set() that refuses it. */
bool set_if_changed(ParamStore& ps, uint16_t id, float value) {
    if (ps.get(id) == value) return false;
    return ps.set(id, value, ParamOrigin::Preset);
}

void apply_snapshot(int count, bool legacy_fx) {
    ParamStore& ps = ParamStore::instance();

    /* What the file names, marked before anything is touched.
     *
     * This is what keeps a stored parameter from being driven through its
     * default on the way to its value. The rebuild detectors above would see
     * two changes and are entitled to act on both — and the first of them is
     * for a configuration nobody asked for, on the block where the load is
     * already at its most expensive. */
    memset(s_named, 0, sizeof(s_named));
    for (int i = 0; i < count; ++i) {
        const uint16_t id = s_pairs[i].id;
        if (id < osynth::PID_SPACE_END) s_named[id >> 5] |= 1u << (id & 31);
    }

    /* Defaults, for everything the file leaves out — which is most of the
     * store, since presets save sparsely, and almost all of it is already
     * sitting there. */
    const size_t n = ps.listIds(s_ids, ParamStore::kMaxParams);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t id = s_ids[i];
        if (skip_id(id)) continue;
        if (id_named(id)) continue;
        const ParamDesc* d = ps.describe(id);
        if (d != nullptr) set_if_changed(ps, id, d->def);
    }

    /* Then the file's own values, still in file order: a file may name two
     * parameters one of whose listeners reads the other, and that ordering was
     * the file's to decide when it was written. */
    for (int i = 0; i < count; ++i) {
        if (skip_id(s_pairs[i].id)) continue;
        /* unregistered ids (older firmware, foreign engine) fail silently */
        set_if_changed(ps, s_pairs[i].id, s_pairs[i].val);
    }
    /* After the pairs, never before: the inference reads the mix values the
     * file just set. */
    if (legacy_fx) legacy_fx_enable();
}""",
)

ed.save("apply each parameter once, and only when it changes")
