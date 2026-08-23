#!/usr/bin/env python3
"""chord.restrike: how a live setting change lands on a held chord (S41f).

S41b re-voiced a held chord by playing only the difference — tones that
survived were left alone, so a knob drag morphed the chord instead of
machine-gunning it. That is the right default and it is not the only thing
anyone wants: re-striking the whole chord is how you hear a change *land*,
and on a percussive patch the difference is the entire effect.

So it becomes a setting, and the diff work earns a second job: deciding
whether the chord changed *at all*. Without that test a re-strike would fire
on every unrelated parameter write — strum, the route, a p-locked value that
happens to sit in the block — and a sustained chord would retrigger for
reasons the player could not see.

Kept per the project's intermediary-artifacts policy. Idempotent.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

HEADER_EDITS = [
    ("""#define CHORD_PID_REV      0x0452 /* int, read-only user-set revision */

/* The whole block, for the preset system's range tests. */
#define CHORD_PID_FIRST 0x0440
#define CHORD_PID_LAST  0x0452""",
     """#define CHORD_PID_REV      0x0452 /* int, read-only user-set revision */
#define CHORD_PID_RESTRIKE 0x0453 /* enum changed / all, on a live change */

/* The whole block, for the preset system's range tests. */
#define CHORD_PID_FIRST 0x0440
#define CHORD_PID_LAST  0x0453"""),

    ("""enum { CHORD_KEYS_POLY = 0, CHORD_KEYS_MONO };""",
     """enum { CHORD_KEYS_POLY = 0, CHORD_KEYS_MONO };
enum { CHORD_RESTRIKE_CHANGED = 0, CHORD_RESTRIKE_ALL };"""),

    (""" * Note-off correctness""",
     """ * Live changes
 *
 * Every setting here applies to the chord already being held, not just to the
 * next one — set the root mid-chord and the chord moves. `chord.restrike`
 * decides how:
 *
 *   changed  play only the tones the change added, and leave the ones that
 *            survived completely alone. Dragging a control through five values
 *            morphs the chord instead of retriggering every voice five times.
 *   all      release the chord and play it again whole. How you hear a change
 *            *land*, and on a percussive patch the whole point.
 *
 * Either way a change that does not move a single tone emits nothing at all,
 * which is what keeps an unrelated write — strum, the route, a parameter lock
 * on something else in the block — from retriggering a sustained chord.
 *
 * ---------------------------------------------------------------------------
 * Note-off correctness"""),
]

CPP_EDITS = [
    ("""    BASS, STRUM, STRUMDIR, VEL, LEAD, RANGE, ROUTE, KEYS, REV,
    P_COUNT""",
     """    BASS, STRUM, STRUMDIR, VEL, LEAD, RANGE, ROUTE, KEYS, REV, RESTRIKE,
    P_COUNT"""),

    ("""const char* const kKeysNames[] = {"poly", "mono"};""",
     """const char* const kKeysNames[] = {"poly", "mono"};
const char* const kRestrikeNames[] = {"changed", "all"};"""),

    ("""    s_params[n++] = {CHORD_PID_REV, "chord.rev", ParamType::Int,
                     ParamCurve::Linear, 0.0f, 16777215.0f, 0.0f, nullptr, 0};""",
     """    s_params[n++] = {CHORD_PID_REV, "chord.rev", ParamType::Int,
                     ParamCurve::Linear, 0.0f, 16777215.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_RESTRIKE, "chord.restrike", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, kRestrikeNames, 2};"""),

    # --- the re-voice itself ---------------------------------------------
    ("""    taskENTER_CRITICAL(&s_lock);
    Held& h = s_held[slot];
    if (!h.used || h.key != key) { /* released while we were building */
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    /* Anything of this key still waiting on the strum clock describes the old
     * chord and would arrive as a stray note. */
    for (int i = 0; i < kMaxPending; ++i) {
        if (s_pend[i].used && s_pend[i].slot == (int8_t)slot) {
            s_pend[i].used = false;
        }
    }

    /* Release what left the chord. A tone present in both keeps the reference
     * it already holds, so nothing has to be added back for it below. */
    for (int i = 0; i < old_count && n < CHORD_MAX_NOTES; ++i) {
        if ((old_emitted & (uint16_t)(1u << i)) == 0) continue;
        bool kept = false;
        for (int j = 0; j < count; ++j) {
            if (tone[j] == (int)old_note[i]) kept = true;
        }
        if (kept) continue;
        if (!ref_release(old_note[i])) continue;""",
     """    taskENTER_CRITICAL(&s_lock);
    Held& h = s_held[slot];
    if (!h.used || h.key != key) { /* released while we were building */
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    /* Did the chord actually move? Both tone lists are built ascending, so
     * comparing them element by element compares the sets.
     *
     * This is what makes the listener safe to point at the whole block. A
     * write that changes no pitch — strum, the strum direction, the route, a
     * parameter lock on something else in the range — leaves here having done
     * nothing, which matters far more in `all` mode: without it a sustained
     * chord would retrigger for reasons the player could not see. */
    bool changed = count != (int)old_count;
    for (int i = 0; !changed && i < count; ++i) {
        if (tone[i] != (int)old_note[i]) changed = true;
    }
    if (!changed) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    /* Play it again whole, rather than only what the change added. Reference
     * counting still has the last word: a tone another key is also holding
     * does not fall to zero, so it is not restruck and not cut — which is the
     * correct answer, since that key never asked for anything. */
    const bool restrike = pi(RESTRIKE) == CHORD_RESTRIKE_ALL;

    /* Anything of this key still waiting on the strum clock describes the old
     * chord and would arrive as a stray note. */
    for (int i = 0; i < kMaxPending; ++i) {
        if (s_pend[i].used && s_pend[i].slot == (int8_t)slot) {
            s_pend[i].used = false;
        }
    }

    /* Release what left the chord — or all of it, when the whole chord is
     * about to be played again. A tone present in both and kept kept its
     * reference too, so nothing has to be added back for it below. */
    for (int i = 0; i < old_count && n < CHORD_MAX_NOTES; ++i) {
        if ((old_emitted & (uint16_t)(1u << i)) == 0) continue;
        bool kept = false;
        if (!restrike) {
            for (int j = 0; j < count; ++j) {
                if (tone[j] == (int)old_note[i]) kept = true;
            }
        }
        if (kept) continue;
        if (!ref_release(old_note[i])) continue;"""),

    ("""    for (int i = 0; i < count; ++i) {
        bool already = false;
        for (int j = 0; j < old_count; ++j) {
            if ((old_emitted & (uint16_t)(1u << j)) &&
                (int)old_note[j] == tone[i]) {
                already = true;
            }
        }
        if (already) continue;""",
     """    for (int i = 0; i < count; ++i) {
        bool already = false;
        if (!restrike) {
            for (int j = 0; j < old_count; ++j) {
                if ((old_emitted & (uint16_t)(1u << j)) &&
                    (int)old_note[j] == tone[i]) {
                    already = true;
                }
            }
        }
        if (already) continue;"""),

    # The strike loop's comment now covers both modes.
    ("""    /* Strike what arrived. `src` is recomputed rather than carried: an""",
     """    /* Strike what arrived — every tone, in `all` mode, since the releases
     * above dropped the whole chord. `src` is recomputed rather than carried:
     * an"""),
]


def edit(rel, edits, guard):
    path = REPO / rel
    src = path.read_text(encoding="utf-8")
    if guard in src:
        print("%s: already patched" % rel)
        return
    for old, new in edits:
        if src.count(old) != 1:
            sys.exit("%s: anchor appears %d times:\n%s"
                     % (rel, src.count(old), old[:80]))
        src = src.replace(old, new, 1)
    path.write_text(src, encoding="utf-8")
    print("%s: patched" % rel)


edit("components/chord/include/chord.h", HEADER_EDITS, "CHORD_PID_RESTRIKE")
edit("components/chord/chord.cpp", CPP_EDITS, "chord.restrike")
