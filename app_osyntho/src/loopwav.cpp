#include "src/loopwav.h"

#include <QtEndian>

#include <cmath>

namespace LoopWav {
namespace {

// IMA ADPCM step/index tables, transcribed from
// components/looper/include/loop_adpcm.h.
constexpr qint16 kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr qint8 kIndexAdj[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                 -1, -1, -1, -1, 2, 4, 6, 8};

// One channel's decoder state. Zeroed at the first byte and never re-seeded —
// see the header for why that is the whole story.
struct Ch {
  qint32 pred = 0;
  qint32 index = 0;

  qint16 decode(quint8 nib) {
    const qint32 step = kStep[index];
    qint32 delta = step >> 3;
    if (nib & 4) delta += step;
    if (nib & 2) delta += step >> 1;
    if (nib & 1) delta += step >> 2;
    pred += (nib & 8) ? -delta : delta;
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    index += kIndexAdj[nib];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    return qint16(pred);
  }
};

void appendLe16(QByteArray& b, quint16 v) {
  char t[2];
  qToLittleEndian<quint16>(v, t);
  b.append(t, 2);
}

void appendLe32(QByteArray& b, quint32 v) {
  char t[4];
  qToLittleEndian<quint32>(v, t);
  b.append(t, 4);
}

void appendSample(QByteArray& out, qint16 s) { appendLe16(out, quint16(s)); }

// Stereo: one byte per frame, left in the high nibble.
QByteArray decodeStereo(const QByteArray& data, quint32 frames) {
  QByteArray pcm;
  const quint32 have = qMin<quint32>(quint32(data.size()), frames);
  pcm.reserve(int(frames) * 4);
  Ch l, r;
  const auto* p = reinterpret_cast<const quint8*>(data.constData());
  for (quint32 i = 0; i < have; ++i) {
    appendSample(pcm, l.decode(p[i] >> 4));
    appendSample(pcm, r.decode(p[i] & 0x0F));
  }
  pcm.append(int((frames - have) * 4), '\0');  // the tail the looper mutes
  return pcm;
}

// Mono: two frames per byte, the earlier one in the high nibble. An odd loop
// length leaves a padding nibble in the last byte; `frames` trims it.
QByteArray decodeMono(const QByteArray& data, quint32 frames) {
  QByteArray pcm;
  const quint32 have = qMin<quint32>(quint32(data.size()) * 2, frames);
  pcm.reserve(int(frames) * 2);
  Ch c;
  const auto* p = reinterpret_cast<const quint8*>(data.constData());
  for (quint32 i = 0; i < have; ++i) {
    const quint8 b = p[i >> 1];
    appendSample(pcm, c.decode((i & 1) == 0 ? (b >> 4) : (b & 0x0F)));
  }
  pcm.append(int((frames - have) * 2), '\0');
  return pcm;
}

// Legacy v1 slot payload: interleaved stereo int16, already PCM and already
// little-endian (it was memcpy'd out of an ESP32).
QByteArray decodeRaw(const QByteArray& data, quint32 frames) {
  const quint32 have = qMin<quint32>(quint32(data.size()) / 4, frames);
  QByteArray pcm = data.left(int(have) * 4);
  pcm.append(int((frames - have) * 4), '\0');
  return pcm;
}

}  // namespace

int channelsFor(int codec) { return codec == kAdpcmMono ? 1 : 2; }

QByteArray decode(const QByteArray& data, int codec, quint32 frames) {
  switch (codec) {
    case kAdpcmStereo: return decodeStereo(data, frames);
    case kAdpcmMono:   return decodeMono(data, frames);
    case kRaw:         return decodeRaw(data, frames);
    default:           return {};
  }
}

QByteArray wrap(const QByteArray& pcm, int channels, int rate) {
  if (pcm.isEmpty() || channels < 1 || rate <= 0) return {};
  QByteArray wav;
  wav.reserve(44 + pcm.size());
  wav.append("RIFF", 4);
  appendLe32(wav, quint32(36 + pcm.size()));
  wav.append("WAVEfmt ", 8);
  appendLe32(wav, 16);                     // PCM fmt chunk size
  appendLe16(wav, 1);                      // format: PCM
  appendLe16(wav, quint16(channels));
  appendLe32(wav, quint32(rate));
  appendLe32(wav, quint32(rate) * quint32(channels) * 2);  // byte rate
  appendLe16(wav, quint16(channels * 2));  // block align
  appendLe16(wav, 16);                     // bits per sample
  wav.append("data", 4);
  appendLe32(wav, quint32(pcm.size()));
  wav.append(pcm);
  return wav;
}

QByteArray build(const QByteArray& data, int codec, quint32 frames, int rate) {
  return wrap(decode(data, codec, frames), channelsFor(codec), rate);
}

void mixInto(Accumulator& acc, const QByteArray& pcm, double gain) {
  const qsizetype n = qMin<qsizetype>(acc.size(), pcm.size() / 2);
  const auto* src = reinterpret_cast<const char*>(pcm.constData());
  qint32* dst = acc.data();
  for (qsizetype i = 0; i < n; ++i) {
    const qint16 s = qFromLittleEndian<qint16>(src + i * 2);
    dst[i] += qint32(std::lround(double(s) * gain));
  }
}

QByteArray buildMixed(const Accumulator& acc, int channels, int rate) {
  if (acc.isEmpty()) return {};
  QByteArray pcm;
  pcm.resize(acc.size() * 2);
  char* out = pcm.data();
  for (qsizetype i = 0; i < acc.size(); ++i) {
    // Clamped once, here — the same place the synth's output stage clamps its
    // own sum, so a mix that would distort on the hardware distorts the same
    // way rather than wrapping.
    const qint32 v = qBound<qint32>(-32768, acc.at(i), 32767);
    qToLittleEndian<qint16>(qint16(v), out + i * 2);
  }
  return wrap(pcm, channels, rate);
}

}  // namespace LoopWav
