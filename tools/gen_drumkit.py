#!/usr/bin/env python3
"""osynth — build a factory drum kit image from a folder of WAV one-shots.

Companion to tools/gen_wavetables.py. Reads a source pack (default layout:
the "opendrums" free pack), trims / resamples / normalises one file per kit
slot and writes a binary kit image (`drumkit.bin`) that the firmware embeds
as .rodata (components/drums/CMakeLists.txt) and parses at boot. The exact
same file format is what the runtime reads from an SD card, so `--out` can
also target /sd/osynth/kits/<name>.okit.

Storage is 8-bit mu-law by default: random-access (a sampler needs to seek
for pitch and start-offset, which rules out IMA-ADPCM's sequential decoder),
one 256-entry LUT to decode, ~38 dB SNR at half the flash traffic of int16.
Flash traffic is the real cost here — the audio task reads this data through
the flash cache.

Each slot picks its own storage rate: a floor tom carries no energy above
1 kHz, a hi-hat carries it to 19 kHz, and paying 44.1 kHz for both wastes
most of the image. The defaults below come from tools/drumkit/survey.py.

Usage
    python tools/gen_drumkit.py --pack opendrums --out build/drumkit.bin
    python tools/gen_drumkit.py --config mykit.json --out kit.okit --sd
    python tools/gen_drumkit.py --pack opendrums --report # sizes only

Requires numpy (+ scipy if available, for a better resampler).
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import zlib

# numpy and the WAV reader are imported lazily, inside the conversion path.
# The firmware build calls this script with --allow-missing to emit a valid
# empty kit when no sample pack is configured, and that path has to work in
# the ESP-IDF python environment, which has no numpy (same constraint that
# keeps tools/gen_wavetables.py on the standard library).
np = None


def _load_deps():
    global np
    import numpy
    np = numpy
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "drumkit"))
    from survey import read_wav
    return read_wav

# ---------------------------------------------------------------------------
# Binary format — mirrored by components/drums/include/drum_kit_fmt.h.
# Any change here must change that header (and bump KIT_VERSION).
# ---------------------------------------------------------------------------
KIT_MAGIC = b"OSYNKIT1"
KIT_VERSION = 1
HEADER_SIZE = 64
SLOT_SIZE = 48
NAME_MAX = 12          # slot name field, NUL-terminated
KIT_NAME_MAX = 24
FMT_ULAW = 0
FMT_PCM16 = 1

# Playback engine limits (components/drums/drums.cpp must agree).
MAX_SLOTS = 32

# ---------------------------------------------------------------------------
# Default kit. One file per slot, chosen from the opendrums pack by ear-proxy:
# shortest clean take for the tight slots, fullest for the sustained ones.
#   file       path relative to --pack
#   secs       hard length cap (the tail is trimmed at -60 dB first)
#   rate       storage sample rate; see the module docstring
#   gain       mix trim baked into the slot header (the app can override)
#   choke      non-zero groups cut each other off (hi-hats)
#   note       MIDI note this slot answers to, General-MIDI drum map
# ---------------------------------------------------------------------------
DEFAULT_KIT_NAME = "opendrums"
DEFAULT_SLOTS = [
    dict(name="kick",       file="kick.wav",       secs=0.40, rate=22050, gain=1.00, choke=0, note=36),
    dict(name="kick.tight", file="kick.tight.wav", secs=0.30, rate=22050, gain=0.95, choke=0, note=35),
    dict(name="snare",      file="snare.wav",      secs=0.45, rate=32000, gain=0.90, choke=0, note=38),
    dict(name="snare.tght", file="snare.tght.wav", secs=0.25, rate=32000, gain=0.85, choke=0, note=40),
    dict(name="stick",      file="stick.wav",      secs=0.25, rate=32000, gain=0.70, choke=0, note=37),
    dict(name="clap",       file="clap.wav",       secs=0.40, rate=32000, gain=0.85, choke=0, note=39),
    dict(name="hh.closed",  file="hh.closed.wav",  secs=0.25, rate=44100, gain=0.75, choke=1, note=42),
    dict(name="hh.pedal",   file="hh.pedal.wav",   secs=0.25, rate=44100, gain=0.70, choke=1, note=44),
    dict(name="hh.open",    file="hh.open.wav",    secs=1.30, rate=32000, gain=0.75, choke=1, note=46),
    dict(name="tom.lo",     file="tom.lo.wav",     secs=0.80, rate=22050, gain=0.90, choke=0, note=41),
    dict(name="tom.mid",    file="tom.mid.wav",    secs=0.90, rate=22050, gain=0.90, choke=0, note=45),
    dict(name="tom.hi",     file="tom.hi.wav",     secs=0.75, rate=22050, gain=0.90, choke=0, note=48),
    dict(name="crash",      file="crash.wav",      secs=2.20, rate=24000, gain=0.80, choke=0, note=49),
    dict(name="ride",       file="ride",           secs=2.20, rate=24000, gain=0.80, choke=0, note=51),
    dict(name="cowbell",    file="cowbell",        secs=0.35, rate=32000, gain=0.70, choke=0, note=56),
    dict(name="shaker",     file="shaker.wav",     secs=0.20, rate=32000, gain=0.65, choke=0, note=70),
]

TAIL_DB = -60.0        # trim the decay where it drops this far below peak
ONSET_DB = -40.0       # strip recorder pre-roll above this level
FADE_MS = 3.0          # fade the trimmed tail out over this long (no click)
PEAK_TARGET = 0.97     # normalise each slot; musical balance lives in `gain`


# ---------------------------------------------------------------------------
# mu-law (ITU-T G.711). Encode/decode kept explicit rather than pulled from
# `audioop`, which was removed in Python 3.13.
# ---------------------------------------------------------------------------
def ulaw_encode(x: np.ndarray) -> np.ndarray:
    """float [-1,1] -> 8-bit mu-law bytes (G.711, bias 33, mu = 255)."""
    BIAS = 0x84
    CLIP = 32635
    s = np.clip(np.rint(x * 32768.0), -32768, 32767).astype(np.int32)
    sign = np.where(s < 0, 0x80, 0x00).astype(np.uint8)
    mag = np.minimum(np.abs(s), CLIP).astype(np.int32) + BIAS
    # exponent = position of the highest set bit above bit 7
    exponent = np.zeros_like(mag)
    for e in range(7, 0, -1):
        exponent = np.where((exponent == 0) & (mag >= (1 << (e + 7))), e,
                            exponent)
    mantissa = (mag >> (exponent + 3)) & 0x0F
    byte = ~(sign | (exponent << 4).astype(np.uint8) | mantissa.astype(np.uint8))
    return (byte & 0xFF).astype(np.uint8)


def ulaw_decode(b: np.ndarray) -> np.ndarray:
    """8-bit mu-law -> float [-1,1]; used to report the encoder's error."""
    BIAS = 0x84
    u = (~b.astype(np.int32)) & 0xFF
    sign = u & 0x80
    exponent = (u >> 4) & 0x07
    mantissa = u & 0x0F
    mag = ((mantissa << 3) + BIAS) << exponent
    mag -= BIAS
    val = np.where(sign != 0, -mag, mag)
    return val.astype(np.float32) / 32768.0


# ---------------------------------------------------------------------------
def resample(x: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    """Band-limited rate conversion; falls back to linear without scipy."""
    if src_rate == dst_rate:
        return x
    try:
        from math import gcd

        from scipy.signal import resample_poly
        g = gcd(int(src_rate), int(dst_rate))
        return resample_poly(x, dst_rate // g, src_rate // g).astype(np.float32)
    except ImportError:
        n = int(round(len(x) * dst_rate / src_rate))
        idx = np.linspace(0, len(x) - 1, n)
        print("  (no scipy: linear resample — expect some aliasing)",
              file=sys.stderr)
        return np.interp(idx, np.arange(len(x)), x).astype(np.float32)


def prepare(read_wav, path: str, max_secs: float, dst_rate: int):
    """WAV file -> (mu-law bytes, frame count) at dst_rate, trimmed+normalised."""
    x, rate = read_wav(path)
    if len(x) == 0:
        raise ValueError("empty file")
    peak = float(np.max(np.abs(x)))
    if peak <= 0:
        raise ValueError("silent file")

    # Strip the recorder's pre-roll so every slot triggers sample-accurately.
    above = np.nonzero(np.abs(x) > peak * 10 ** (ONSET_DB / 20))[0]
    if len(above):
        # Back off a hair so the very start of the transient survives.
        x = x[max(0, int(above[0]) - int(rate * 0.001)):]

    # Trim the decay once it is inaudible, then apply the hard cap.
    win = max(1, int(rate * 0.005))
    n = (len(x) // win) * win
    if n:
        rms = np.sqrt((x[:n].reshape(-1, win) ** 2).mean(axis=1))
        live = np.nonzero(rms > peak * 10 ** (TAIL_DB / 20))[0]
        if len(live):
            x = x[: int((live[-1] + 1) * win)]
    x = x[: int(max_secs * rate)]

    x = resample(x, rate, dst_rate)

    # Normalise for mu-law headroom; musical balance is the slot `gain` field.
    p = float(np.max(np.abs(x)))
    if p > 0:
        x = x * (PEAK_TARGET / p)

    # Fade the (possibly mid-decay) tail so a truncated cymbal cannot click.
    fade = min(len(x), int(FADE_MS * 1e-3 * dst_rate))
    if fade > 1:
        x[-fade:] *= np.linspace(1.0, 0.0, fade, dtype=np.float32)

    enc = ulaw_encode(x)
    err = float(np.sqrt(np.mean((ulaw_decode(enc) - x) ** 2)))
    snr = 20 * np.log10(float(np.sqrt(np.mean(x ** 2))) / err) if err > 0 else 99
    return enc, len(x), snr


def build(pack: str, slots_cfg: list, kit_name: str, verbose=True):
    """Assemble the whole image; returns (bytes, per-slot report rows)."""
    if len(slots_cfg) > MAX_SLOTS:
        raise ValueError(f"{len(slots_cfg)} slots > MAX_SLOTS ({MAX_SLOTS})")
    read_wav = _load_deps()

    blobs, rows = [], []
    data_off = HEADER_SIZE + SLOT_SIZE * len(slots_cfg)
    cursor = data_off

    for s in slots_cfg:
        path = os.path.join(pack, s["file"])
        if not os.path.isfile(path):
            print(f"!! missing: {path} — slot '{s['name']}' left empty",
                  file=sys.stderr)
            rows.append((s["name"], 0, s["rate"], 0, 0.0))
            blobs.append((s, b"", 0))
            continue
        enc, frames, snr = prepare(read_wav, path, s["secs"], s["rate"])
        blobs.append((s, enc.tobytes(), frames))
        rows.append((s["name"], frames, s["rate"], len(enc), snr))
        if verbose:
            print(f"  {s['name']:<11} {frames / s['rate']:5.2f}s @{s['rate']:>5} "
                  f"= {len(enc) / 1024:7.1f} KB   SNR {snr:4.1f} dB   "
                  f"{os.path.basename(s['file'])}")

    # --- slot table + data ------------------------------------------------
    table = b""
    data = b""
    for s, blob, frames in blobs:
        off = cursor if blob else 0
        table += struct.pack(
            "<12sIIIIIffBBBB4s",
            s["name"].encode("ascii")[: NAME_MAX - 1],
            off,
            frames,
            s["rate"],
            int(s.get("loop_start", 0)),
            int(s.get("loop_end", 0)),
            float(s.get("gain", 1.0)),
            float(s.get("pan", 0.0)),
            FMT_ULAW,
            int(s.get("choke", 0)),
            int(s.get("note", 36)),
            0,
            b"\0" * 4,
        )
        data += blob
        cursor += len(blob)
        # Keep every slot's data 4-byte aligned: the reader hands out raw
        # pointers and PCM16 kits would otherwise fault on a misaligned load.
        pad = (-len(data)) % 4
        data += b"\0" * pad
        cursor += pad

    total = HEADER_SIZE + len(table) + len(data)
    body = table + data
    header = struct.pack(
        "<8sHHI24sI20s",
        KIT_MAGIC,
        KIT_VERSION,
        len(slots_cfg),
        total,
        kit_name.encode("utf-8")[: KIT_NAME_MAX - 1],
        zlib.crc32(body) & 0xFFFFFFFF,
        b"\0" * 20,
    )
    assert len(header) == HEADER_SIZE, len(header)
    assert len(table) == SLOT_SIZE * len(slots_cfg), len(table)
    return header + body, rows


def empty_image(kit_name="none"):
    """A valid, slotless image — lets the firmware build without a pack."""
    header = struct.pack("<8sHHI24sI20s", KIT_MAGIC, KIT_VERSION, 0,
                         HEADER_SIZE, kit_name.encode()[:KIT_NAME_MAX - 1],
                         zlib.crc32(b"") & 0xFFFFFFFF, b"\0" * 20)
    return header


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", default="opendrums",
                    help="source folder of WAV one-shots (default: opendrums)")
    ap.add_argument("--config", help="JSON overriding the default slot list: "
                                     '{"name": "...", "slots": [ ... ]}')
    ap.add_argument("--out", default="build/drumkit.bin", help="output image")
    ap.add_argument("--name", help="kit name stored in the header")
    ap.add_argument("--report", action="store_true",
                    help="print the size table without writing anything")
    ap.add_argument("--allow-missing", action="store_true",
                    help="write an empty kit if the pack is absent instead of "
                         "failing (used by the firmware build)")
    args = ap.parse_args()

    slots_cfg = DEFAULT_SLOTS
    kit_name = args.name or DEFAULT_KIT_NAME
    if args.config:
        with open(args.config, encoding="utf-8") as f:
            cfg = json.load(f)
        slots_cfg = cfg.get("slots", DEFAULT_SLOTS)
        kit_name = args.name or cfg.get("name", DEFAULT_KIT_NAME)

    if not os.path.isdir(args.pack):
        msg = f"pack folder not found: {args.pack}"
        if not args.allow_missing:
            print(f"error: {msg}", file=sys.stderr)
            return 1
        print(f"warning: {msg} — writing an empty kit", file=sys.stderr)
        img = empty_image()
        rows = []
    else:
        print(f"building kit '{kit_name}' from {args.pack}")
        img, rows = build(args.pack, slots_cfg, kit_name,
                          verbose=not args.report)

    print(f"\n{len(rows)} slots, image {len(img) / 1024:.1f} KB")
    if args.report:
        return 0

    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(img)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
