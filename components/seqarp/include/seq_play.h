/*
 * osynth — sequencer playback engine (Session 23).
 *
 * Consumes the 96-PPQN tick produced by seqarp.cpp and turns the pattern
 * store (seq_model.h) into notes and drum hits. Runs entirely on the
 * `seq_clk` task; nothing here touches the audio task, and the only shared
 * state with it is the parameter store and the drum bus's trigger ring.
 *
 * Why 96 PPQN and not the MIDI-standard 24: a step at 1/16 is 6 ticks at
 * 24 PPQN, which leaves no resolution inside a step for micro-timing or for
 * spacing 8 ratchets, and 1/16 triplets land on fractional ticks. At 96 a
 * 1/16 step is 24 ticks, micro-timing resolves to ~1/24 of a step (about
 * 5 ms at 120 BPM) and every triplet division is exact. External MIDI clock
 * still arrives at 24 PPQN and is multiplied up — see seqarp.cpp.
 *
 * Each track keeps its own step position, length and division, so polymeter
 * costs nothing: a 12-step bassline against a 16-step drum lane is just two
 * different `length` values.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "seq_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Live locks a track may hold at once. Beyond this the extra locks on a step
 * are ignored rather than silently displacing an active one — an unbalanced
 * apply/restore would leave a parameter stuck at a locked value. */
#define SEQ_TRACK_LOCKS 8

void seq_play_init(void);

/* Cache the ParamStore pointers the tick path reads. Call after every
 * 0x04xx parameter has been registered. */
void seq_play_bind_params(void);

/* One 96-PPQN tick. Fires steps, retriggers ratchets, releases gates. */
void seq_play_tick(void);

/* Transport. seq_play_start(false) resumes where it stopped (MIDI continue),
 * seq_play_start(true) rewinds every track to step 0. Stopping releases held
 * notes and restores every parameter a lock had changed. */
void seq_play_start(bool rewind);
void seq_play_stop(void);
bool seq_play_running(void);

/* Pattern selection. Changes take effect at the next pattern boundary unless
 * `immediate`, which is what a stopped transport wants. */
void seq_play_select_pattern(int pattern, bool immediate);
int seq_play_current_pattern(void);

/* Live "fill" state behind the FILL / NOT FILL trig conditions. */
void seq_play_set_fill(bool on);

/* Playhead of a track, or -1 if the transport is stopped. */
int seq_play_position(int track);

/* Live recording. While the transport runs the note lands on the step
 * nearest to now (quantised by seq.quant); stopped, it lands on the edit
 * step and advances it — step input on a device with no screen. Returns the
 * step it wrote to, or -1 if it wrote nothing. */
int seq_play_record_note(uint8_t note, uint8_t velocity);

/* The same, for a drum hit addressed by kit slot: the app's pads and MIDI
 * notes on the drum channel, neither of which passes the note tap. Picks the
 * drum lane that can honestly play this slot — the edited track first — and
 * records nothing if there is none. See the implementation for the full rule.
 * Returns the step it wrote to, or -1. */
int seq_play_record_drum(int slot, uint8_t velocity);

#ifdef __cplusplus
}
#endif
