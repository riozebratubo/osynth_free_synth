#!/usr/bin/env python3
"""Re-voice held chords the moment a setting changes (S41b).

A chord was built at key-down and never revisited, so changing the key, the
scale, the size or the voicing was inaudible until the next note: you set the
root and kept hearing the old chord until you let go. This adds a ParamStore
listener that rebuilds what is held and plays only the difference.

Kept per the project's intermediary-artifacts policy. Idempotent.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
path = REPO / "components/chord/chord.cpp"

EDITS = [
    # --- the held record keeps the velocity it was struck at --------------
    ("""struct Held {
    uint8_t key;
    uint8_t count;""",
     """struct Held {
    uint8_t key;
    /* The velocity the key was struck at. Kept because a re-voice has to
     * strike the tones a setting change *added* at the level the player
     * actually played, and by then the note-on is long gone. */
    uint8_t vel;
    uint8_t count;"""),

    # --- one owner of the velocity taper ---------------------------------
    ("""/* s_lock held. Adds a reference and reports whether the tone has to be""",
     """/* Velocity for tone `i` of `count`, tapering toward the top of the chord:
 * quietest on top, which is how a hand actually plays one and what keeps a
 * stacked 13th from sounding like an organ. Never below 1 — 0 is a note-off
 * in MIDI. Shared by the initial strike and by a re-voice, so a chord that
 * changes under a held key keeps the same shape it was struck with. */
uint8_t tone_velocity(int i, int count, uint8_t key_vel) {
    float v = (float)key_vel;
    const float fall = pv(VEL);
    if (count > 1 && fall > 0.0f) {
        v *= 1.0f - fall * ((float)i / (float)(count - 1));
    }
    int iv = (int)(v + 0.5f);
    if (iv < 1) iv = 1;
    if (iv > 127) iv = 127;
    return (uint8_t)iv;
}

/* s_lock held. Adds a reference and reports whether the tone has to be"""),

    # --- chord_key_on uses it --------------------------------------------
    ("""    /* Velocity falloff: quietest at the top, which is how a hand actually
     * plays a chord and what keeps a stacked 13th from sounding like an
     * organ. Never below 1 — 0 is a note-off in MIDI. */
    uint8_t tvel[CHORD_MAX_NOTES];
    const float fall = pv(VEL);
    for (int i = 0; i < count; ++i) {
        float v = (float)vel;
        if (count > 1 && fall > 0.0f) {
            v *= 1.0f - fall * ((float)i / (float)(count - 1));
        }
        int iv = (int)(v + 0.5f);
        if (iv < 1) iv = 1;
        if (iv > 127) iv = 127;
        tvel[i] = (uint8_t)iv;
    }
""",
     """    uint8_t tvel[CHORD_MAX_NOTES];
    for (int i = 0; i < count; ++i) tvel[i] = tone_velocity(i, count, vel);
"""),

    ("""    Held& h = s_held[slot];
    h.used = true;
    h.key = note;
    h.pre = want_pre;""",
     """    Held& h = s_held[slot];
    h.used = true;
    h.key = note;
    h.vel = vel;
    h.pre = want_pre;"""),
]

REVOICE = '''
/* ---- live re-voicing ----------------------------------------------------
 *
 * A chord is built when the key goes down. Without this, changing the key,
 * the scale, the chord size or the voicing was inaudible until the *next*
 * note: you would set the root and go on hearing the old chord until you let
 * go — which on a page whose whole point is choosing a key made the controls
 * feel broken.
 *
 * So a change rebuilds what is held and plays only the difference. Tones that
 * survive are left completely alone — not released and re-struck — because
 * dragging a knob through five values would otherwise retrigger every voice
 * five times, and a sustained chord would machine-gun instead of morphing.
 *
 * One held key at a time, so the emission buffer stays one chord wide on the
 * caller's stack rather than kMaxKeys chords wide. That caller is whichever
 * task wrote the parameter — the BLE control task for an app edit, the clock
 * task for a parameter lock — which is the same set of tasks that already
 * play notes through here.
 */
void revoice_entry(int slot) {
    uint8_t key, key_vel;
    bool pre;
    uint8_t old_note[CHORD_MAX_NOTES];
    int old_count;
    uint16_t old_emitted;

    taskENTER_CRITICAL(&s_lock);
    if (!s_held[slot].used) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    key = s_held[slot].key;
    key_vel = s_held[slot].vel;
    pre = s_held[slot].pre;
    old_count = s_held[slot].count;
    old_emitted = s_held[slot].emitted;
    memcpy(old_note, s_held[slot].note, sizeof(old_note));
    taskEXIT_CRITICAL(&s_lock);

    /* Outside the lock, because build_chord() takes it to read the user set. */
    int tone[CHORD_MAX_NOTES];
    int root_idx = 0;
    int count;
    if (enabled()) {
        count = build_chord(key, tone, &root_idx);
    } else {
        /* Chord mode switched off under a held key. It goes back to being the
         * note it is rather than falling silent until the player lets go —
         * "off" should mean the keyboard plays notes, including the one
         * already down. */
        tone[0] = (int)key;
        count = 1;
        root_idx = 0;
    }

    EmitItem items[CHORD_MAX_NOTES * 2];
    int n = 0;

    taskENTER_CRITICAL(&s_lock);
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
        if (!ref_release(old_note[i])) continue;
        items[n].note = old_note[i];
        items[n].vel = 0;
        items[n].on = false;
        items[n].pre = pre;
        items[n].src = MIDI_NOTE_CHORD_TONE;
        ++n;
    }

    h.count = (uint8_t)count;
    h.emitted = 0;
    for (int i = 0; i < count; ++i) {
        h.note[i] = (uint8_t)tone[i];
        h.emitted |= (uint16_t)(1u << i);
    }

    /* Strike what arrived. `src` is recomputed rather than carried: an
     * inversion can move which tone stands for the played key, and the note
     * tap reads that to decide what the recorder stores. */
    for (int i = 0; i < count; ++i) {
        bool already = false;
        for (int j = 0; j < old_count; ++j) {
            if ((old_emitted & (uint16_t)(1u << j)) &&
                (int)old_note[j] == tone[i]) {
                already = true;
            }
        }
        if (already) continue;
        if (!ref_add((uint8_t)tone[i])) continue;
        items[n].note = (uint8_t)tone[i];
        items[n].vel = tone_velocity(i, count, key_vel);
        items[n].on = true;
        items[n].pre = pre;
        items[n].src = i == root_idx ? MIDI_NOTE_CHORD_ROOT
                                     : MIDI_NOTE_CHORD_TONE;
        ++n;
    }
    taskEXIT_CRITICAL(&s_lock);

    flush(items, n);
}

void revoice_all() {
    for (int i = 0; i < kMaxKeys; ++i) revoice_entry(i);
}

/* Every 0x044x parameter, plus the two seqarp ones chord.follow mirrors.
 *
 * Deliberately not filtered down to the settings that move a pitch. A
 * parameter that does not — strum, the strum direction, the route — produces
 * an identical chord, the diff above comes out empty and nothing is emitted,
 * so the cost of being generous is one comparison per held tone. Filtering
 * would only add a list to forget to update.
 *
 * Runs on whichever task wrote the parameter, synchronously. Nothing here
 * writes a parameter, so it cannot re-enter. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void* ctx) {
    (void)value;
    (void)origin;
    (void)ctx;
    const bool ours = (id >= CHORD_PID_FIRST && id <= CHORD_PID_LAST) ||
                      id == SEQ_PID_SCALE || id == SEQ_PID_ROOT;
    if (!ours) return;
    revoice_all();
}

'''


def main():
    src = path.read_text(encoding="utf-8")
    if "revoice_entry" in src:
        print("already patched")
        return
    for old, new in EDITS:
        if src.count(old) != 1:
            sys.exit("anchor appears %d times:\n%s" % (src.count(old), old[:70]))
        src = src.replace(old, new, 1)

    anchor = "/* Chord mode is only *on* when the toggle says so."
    if src.count(anchor) != 1:
        sys.exit("revoice anchor appears %d times" % src.count(anchor))
    # revoice_* needs enabled(), so it goes after it; the listener goes with it.
    marker = "} // namespace"
    if src.count(marker) != 1:
        sys.exit("namespace end appears %d times" % src.count(marker))
    src = src.replace(marker, REVOICE.lstrip("\n") + marker, 1)

    # register the listener at init
    init_anchor = """    /* Last, so the router can never see a half-built component: the hook is
     * a plain pointer assignment and the first note may arrive on another
     * task the instant it lands. */
    midi_set_chord_hook(router_hook, nullptr);"""
    if src.count(init_anchor) != 1:
        sys.exit("init anchor appears %d times" % src.count(init_anchor))
    src = src.replace(init_anchor, """    /* Re-voice held chords when a setting moves (see revoice_entry). Failing
     * to get a listener slot is not fatal: chord mode works, it just goes
     * back to applying a change on the next note. */
    if (ps.addListener(param_listener, nullptr) < 0) {
        ESP_LOGW(TAG, "no listener slot — settings apply on the next note");
    }

""" + init_anchor, 1)
    path.write_text(src, encoding="utf-8")
    print("patched")


main()
