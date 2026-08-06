#ifndef LOOPWAV_H
#define LOOPWAV_H

#include <QByteArray>
#include <QList>

#include <cstdint>

// Turns downloaded osynth loop tracks into WAV file images. Pure, no Qt beyond
// the containers and no I/O — the caller writes the bytes wherever the
// platform wants them.
//
// The synth sends tracks in the codec they are stored in (see OP_LOOP_DUMP in
// ble/synthprotocol.h): IMA-ADPCM packed osynth's way, or — for slot blobs
// written by pre-S20 firmware — raw stereo int16. The ADPCM packing is *not*
// what a WAV ADPCM chunk holds: there are no block headers anywhere, because
// the looper's transport only ever starts or wraps at the loop start, so a
// zeroed decoder state at byte 0 is the whole seek story. Nothing off the
// shelf reads that, which is why the decode lives here. It is a transcription
// of components/looper/include/loop_adpcm.h, and tools/olt2wav.py is the same
// thing again in Python for files pulled off the card by hand — the three have
// to agree.
namespace LoopWav {

// Codec numbers as they travel on the wire (SynthProto::LoopCodec).
enum Codec { kRaw = 0, kAdpcmStereo = 1, kAdpcmMono = 2 };

// Channels a codec decodes to: mono sets really are one channel. The looper
// plays a mono track to both outputs at the same gain, so widening it here
// would only double the file for nothing.
int channelsFor(int codec);

// One track as interleaved 16-bit PCM, exactly `frames` frames long.
//
// `data` may legitimately be shorter than the loop: a punch-in the transport
// stopped keeps what it recorded and the looper plays silence for the rest of
// the pass, so the missing tail is padded here rather than left as a track
// that ends early and drifts out of step with its siblings. Empty if `codec`
// is not one of the three above.
QByteArray decode(const QByteArray& data, int codec, quint32 frames);

// PCM in a 16-bit WAV container.
QByteArray wrap(const QByteArray& pcm, int channels, int rate);

// decode() + wrap(), for exporting a single track untouched.
QByteArray build(const QByteArray& data, int codec, quint32 frames, int rate);

// --- mixdown --------------------------------------------------------------
// Tracks are summed one at a time into a wide accumulator rather than held
// decoded side by side: a full eight-track set at 16-bit would be eight times
// the PCM in memory at once, and only the sum is ever wanted.
//
// The sum reproduces what the looper's own mix does (looper.cpp): each track's
// samples scaled by its loop.lvlN and added, with the clamp happening once at
// the end — so a mix that would clip on the synth's output clips the same way
// here instead of wrapping around.
using Accumulator = QList<qint32>;  // interleaved, `channels` per frame

// Sums one decoded track in at `gain` (its loop.lvlN, 0..1). Only the samples
// the two have in common are touched, so a short `pcm` cannot run off the end
// of a mix sized from the loop length.
void mixInto(Accumulator& acc, const QByteArray& pcm, double gain);

// Clamps the accumulator to 16-bit and wraps it. Empty if `acc` is.
QByteArray buildMixed(const Accumulator& acc, int channels, int rate);

}  // namespace LoopWav

#endif  // LOOPWAV_H
