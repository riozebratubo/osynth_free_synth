#!/usr/bin/env python3
"""Route the arpeggiator's own steps as final when its held list already holds
chord tones (S41).

The bug this fixes: with chord.route = pre-arp, chord mode expands a key into
tones *before* the note tap, so the arpeggiator's held list is the chord. Its
steps then re-entered the router as ordinary notes and were expanded a second
time — a chord stacked on every note of the arpeggio — and its note-offs, which
frequently carry the played key's own pitch, tore down the chord the player was
still holding.

The fix is one bit per held note: whether it arrived already expanded (the note
tap's `src`). A step played out of such a note is routed with allow_chord =
false. In the post-arp routing the same list holds played keys, the bit is
false, and each step becomes a block chord as intended.

Kept per the project's intermediary-artifacts policy. Idempotent.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
path = REPO / "components/seqarp/seqarp.cpp"

EDITS = [
    # --- the held-note record -------------------------------------------
    ("""struct HeldNote {
    uint8_t note;
    uint8_t vel;
    bool down; /* key physically held (false = kept latched by arp.hold) */
};""",
     """struct HeldNote {
    uint8_t note;
    uint8_t vel;
    bool down; /* key physically held (false = kept latched by arp.hold) */
    /* This note reached the tap already expanded by chord mode (S41), i.e.
     * the list is a chord's tones rather than played keys. A step played out
     * of one is routed back as final, so chord mode does not expand it a
     * second time — see midi.h's note-tap `src`. */
    bool from_chord;
};"""),

    # --- emission ---------------------------------------------------------
    ("""inline void emit_on(uint8_t note, uint8_t vel) {
    midi_route_channel_message(0x90, note, vel);
}
inline void emit_off(uint8_t note) { midi_route_channel_message(0x80, note, 0); }""",
     """/* `final` means the note needs no further expansion: it came out of chord
 * mode already, so the router must play it as written. False lets chord mode
 * turn the step into a block chord, which is what the post-arp routing is. */
inline void emit_on(uint8_t note, uint8_t vel, bool final) {
    midi_route_note(0x90, note, vel, !final);
}
inline void emit_off(uint8_t note, bool final) {
    midi_route_note(0x80, note, 0, !final);
}"""),

    # --- the pending note-off table --------------------------------------
    ("""struct Pending {
    uint8_t note;
    int16_t ticks; /* emits the note-off when it reaches 0 */
};""",
     """struct Pending {
    uint8_t note;
    int16_t ticks; /* emits the note-off when it reaches 0 */
    /* Carried from the note-on: a release has to take the same route the
     * press did, or a final note's note-off would be offered to chord mode
     * and would match — and release — a chord the player is still holding. */
    bool final;
};"""),

    ("""void pend_push(uint8_t note, int gate_ticks) {
    if (s_pend_count == kMaxPending) { /* never leak a note-off */
        emit_off(s_pend[0].note);
        for (int i = 1; i < kMaxPending; ++i) s_pend[i - 1] = s_pend[i];
        --s_pend_count;
    }
    s_pend[s_pend_count].note = note;
    s_pend[s_pend_count].ticks = (int16_t)gate_ticks;
    ++s_pend_count;
}""",
     """void pend_push(uint8_t note, int gate_ticks, bool final) {
    if (s_pend_count == kMaxPending) { /* never leak a note-off */
        emit_off(s_pend[0].note, s_pend[0].final);
        for (int i = 1; i < kMaxPending; ++i) s_pend[i - 1] = s_pend[i];
        --s_pend_count;
    }
    s_pend[s_pend_count].note = note;
    s_pend[s_pend_count].ticks = (int16_t)gate_ticks;
    s_pend[s_pend_count].final = final;
    ++s_pend_count;
}"""),

    ("""        if (--s_pend[i].ticks <= 0) {
            emit_off(s_pend[i].note);""",
     """        if (--s_pend[i].ticks <= 0) {
            emit_off(s_pend[i].note, s_pend[i].final);"""),

    ("""void pend_flush() {
    for (int i = 0; i < s_pend_count; ++i) emit_off(s_pend[i].note);
    s_pend_count = 0;
}""",
     """void pend_flush() {
    for (int i = 0; i < s_pend_count; ++i) {
        emit_off(s_pend[i].note, s_pend[i].final);
    }
    s_pend_count = 0;
}"""),

    # --- arp_step: carry the bit through the snapshot --------------------
    ("""    uint8_t notes[kMaxHeld], vels[kMaxHeld];
    int n;
    taskENTER_CRITICAL(&s_lock);
    n = s_held_count;
    for (int i = 0; i < n; ++i) {
        notes[i] = s_held[i].note;
        vels[i] = s_held[i].vel;
    }
    taskEXIT_CRITICAL(&s_lock);""",
     """    uint8_t notes[kMaxHeld], vels[kMaxHeld];
    /* One flag for the whole step rather than per note: chord mode expands a
     * key into tones all at once, so a held list is either all chord tones or
     * none. Taking it from the note the step actually plays keeps that true
     * even in the window where a list is half replaced. */
    bool from_chord[kMaxHeld];
    int n;
    taskENTER_CRITICAL(&s_lock);
    n = s_held_count;
    for (int i = 0; i < n; ++i) {
        notes[i] = s_held[i].note;
        vels[i] = s_held[i].vel;
        from_chord[i] = s_held[i].from_chord;
    }
    taskEXIT_CRITICAL(&s_lock);"""),

    ("""    if (mode != ARP_PLAYED) sort_pairs(notes, vels, n); /* arrival order else */""",
     """    /* sort_pairs moves notes and velocities together; from_chord is indexed
     * the same way, so it has to travel with them. */
    if (mode != ARP_PLAYED) sort_triples(notes, vels, from_chord, n);"""),

    ("""    emit_on((uint8_t)note, vels[i]);
    pend_push((uint8_t)note, gate_ticks);""",
     """    emit_on((uint8_t)note, vels[i], from_chord[i]);
    pend_push((uint8_t)note, gate_ticks, from_chord[i]);"""),

    # --- the tap ---------------------------------------------------------
    ("""bool note_tap(uint8_t note, uint8_t vel, bool on, bool chord_tone, void* ctx) {""",
     """bool note_tap(uint8_t note, uint8_t vel, bool on, int src, void* ctx) {"""),

    ("""        record = pi(SEQ_MODE) == SEQ_REC && !chord_tone;""",
     """        record = pi(SEQ_MODE) == SEQ_REC && src != MIDI_NOTE_CHORD_TONE;
        const bool from_chord = src != MIDI_NOTE_PLAYED;"""),

    ("""            const int idx = held_find(note);
            if (idx >= 0) { /* retrigger: refresh */
                s_held[idx].vel = vel;
                s_held[idx].down = true;
                own_set(note);
                consumed = true;
            } else if (s_held_count < kMaxHeld) {
                kick = s_held_count == 0;
                s_held[s_held_count].note = note;
                s_held[s_held_count].vel = vel;
                s_held[s_held_count].down = true;
                ++s_held_count;""",
     """            const int idx = held_find(note);
            if (idx >= 0) { /* retrigger: refresh */
                s_held[idx].vel = vel;
                s_held[idx].down = true;
                s_held[idx].from_chord = from_chord;
                own_set(note);
                consumed = true;
            } else if (s_held_count < kMaxHeld) {
                kick = s_held_count == 0;
                s_held[s_held_count].note = note;
                s_held[s_held_count].vel = vel;
                s_held[s_held_count].down = true;
                s_held[s_held_count].from_chord = from_chord;
                ++s_held_count;"""),
]


def main():
    src = path.read_text(encoding="utf-8")
    if "from_chord" in src:
        print("already patched")
        return
    for old, new in EDITS:
        if src.count(old) != 1:
            sys.exit("anchor appears %d times:\n%s" % (src.count(old), old[:80]))
        src = src.replace(old, new, 1)
    path.write_text(src, encoding="utf-8")
    print("patched")


main()
