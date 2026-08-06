#!/usr/bin/env python3
"""Decode osynth looper files to WAV.

Two formats, both IMA-ADPCM but packed osynth's way rather than the way WAV
does it, so nothing off the shelf reads them:

  *.olt   live streamed tracks (/sd/osynth/liveN.olt). Nothing but nibbles —
          no header at all, because the length and format are the live set's
          own state and writing a second copy to the card would only be one
          more thing to keep honest. So the packing has to be told here:
          --mono (the firmware default) or --stereo.

  *.olp   save slots (/sd/osynth/loopN.olp, or the flash region). These do
          carry a header, so everything is read from the file and one WAV per
          stored track comes out.

Packing (loop_adpcm.h): stereo is one byte per frame, left in the high nibble;
mono is two frames per byte, the earlier frame in the high nibble. Decoder
state starts at zero on the first byte and is never re-seeded — the transport
only ever starts or wraps at the loop start, which is the whole reason the
format can get away with no block headers.

A track that was stopped part way through its pass ends early: the firmware
writes a short fade to digital silence and then stops writing, and the player
serves zeros for the rest. That is why an .olt can be shorter than the loop.

Usage:
    python tools/olt2wav.py live0.olt                 # mono, 48 kHz
    python tools/olt2wav.py --stereo live0.olt
    python tools/olt2wav.py loop0.olp                 # header decides
    python tools/olt2wav.py -o out/ *.olt
"""

import argparse
import os
import struct
import sys
import wave

# --- IMA ADPCM, transcribed from components/looper/include/loop_adpcm.h ---

STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


class Ch:
    """One channel's decoder state: predictor and step index, both zero at the
    start of a track."""

    __slots__ = ("pred", "index")

    def __init__(self):
        self.pred = 0
        self.index = 0

    def decode(self, nib):
        step = STEP[self.index]
        delta = step >> 3
        if nib & 4:
            delta += step
        if nib & 2:
            delta += step >> 1
        if nib & 1:
            delta += step >> 2
        self.pred += -delta if nib & 8 else delta
        if self.pred > 32767:
            self.pred = 32767
        if self.pred < -32768:
            self.pred = -32768
        self.index += INDEX_ADJ[nib]
        if self.index < 0:
            self.index = 0
        if self.index > 88:
            self.index = 88
        return self.pred


def decode_stereo(data):
    """One byte per frame: left in the high nibble, right in the low."""
    left, right = Ch(), Ch()
    out = bytearray(4 * len(data))
    pack = struct.Struct("<hh").pack_into
    for i, b in enumerate(data):
        pack(out, 4 * i, left.decode(b >> 4), right.decode(b & 0x0F))
    return bytes(out), 2


def decode_mono(data, frames=None):
    """Two frames per byte, the earlier one in the high nibble. `frames`
    trims the padding nibble an odd loop length leaves in the last byte."""
    ch = Ch()
    total = 2 * len(data) if frames is None else min(frames, 2 * len(data))
    out = bytearray(2 * total)
    pack = struct.Struct("<h").pack_into
    for i in range(total):
        b = data[i >> 1]
        pack(out, 2 * i, ch.decode(b >> 4 if (i & 1) == 0 else b & 0x0F))
    return bytes(out), 1


def decode_raw(data):
    """Legacy v1 slot payload: raw interleaved stereo int16, already PCM."""
    return data[: len(data) - (len(data) % 4)], 2


def write_wav(path, pcm, channels, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    frames = len(pcm) // (2 * channels)
    print("%s: %d frames, %.2f s, %s" %
          (path, frames, frames / float(rate),
           "stereo" if channels == 2 else "mono"))


# --- .olp slot blobs (loop_store.cpp: StoreHdr) ---

HDR = struct.Struct("<4sBBBBIII12x")  # 32 bytes
CODEC_RAW, CODEC_ADPCM, CODEC_ADPCM_MONO = 0, 1, 2


def convert_olp(path, outdir):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < HDR.size:
        sys.exit("%s: too short to hold a header" % path)
    magic, version, filled, tracks, codec, frames, rate, tbytes = \
        HDR.unpack_from(blob, 0)
    if magic != b"OSL1":
        sys.exit("%s: not an osynth slot blob (magic %r)" % (path, magic))
    if version == 1:
        codec = CODEC_RAW  # v1 predates the codec byte, which was reserved 0
    stem = os.path.splitext(os.path.basename(path))[0]
    print("%s: v%d, %d frames @ %d Hz, tracks 0x%02x, codec %d, %d B/track" %
          (path, version, frames, rate, filled, codec, tbytes))
    pos = HDR.size
    for t in range(tracks):
        if not (filled >> t) & 1:
            continue  # only stored tracks are in the blob, packed in order
        chunk = blob[pos:pos + tbytes]
        pos += tbytes
        if len(chunk) < tbytes:
            print("  track %d: truncated, decoding what is there" % (t + 1))
        if codec == CODEC_ADPCM_MONO:
            pcm, ch = decode_mono(chunk, frames)
        elif codec == CODEC_ADPCM:
            pcm, ch = decode_stereo(chunk)
        elif codec == CODEC_RAW:
            pcm, ch = decode_raw(chunk)
        else:
            sys.exit("%s: unknown codec %d" % (path, codec))
        write_wav(os.path.join(outdir, "%s-t%d.wav" % (stem, t + 1)), pcm, ch,
                  rate)


def convert_olt(path, outdir, mono, rate):
    with open(path, "rb") as f:
        data = f.read()
    if not data:
        sys.exit("%s: empty" % path)
    pcm, ch = decode_mono(data) if mono else decode_stereo(data)
    stem = os.path.splitext(os.path.basename(path))[0]
    write_wav(os.path.join(outdir, stem + ".wav"), pcm, ch, rate)


def main():
    p = argparse.ArgumentParser(
        description="Decode osynth .olt / .olp loop files to WAV.")
    p.add_argument("files", nargs="+", help=".olt tracks or .olp slot blobs")
    p.add_argument("-o", "--outdir", default=".",
                   help="where the WAVs go (default: here)")
    p.add_argument("--stereo", action="store_true",
                   help=".olt only: the take was stereo (loop.mono off)")
    p.add_argument("--mono", action="store_true",
                   help=".olt only: the take was mono (the default)")
    p.add_argument("--rate", type=int, default=48000,
                   help=".olt only: sample rate (default 48000)")
    a = p.parse_args()
    if a.stereo and a.mono:
        sys.exit("--mono and --stereo are the same switch; pick one")
    if a.outdir and not os.path.isdir(a.outdir):
        os.makedirs(a.outdir)
    for path in a.files:
        if path.lower().endswith(".olp"):
            convert_olp(path, a.outdir)
        else:
            convert_olt(path, a.outdir, not a.stereo, a.rate)


if __name__ == "__main__":
    main()
