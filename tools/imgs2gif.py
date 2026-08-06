#!/usr/bin/env python3
"""Build an animated GIF from every image in a folder.

Frames come out in natural name order, so screenshot2 .. screenshot10 land in
the order a human reads them rather than the order a plain string sort gives.

Timing is given either as the whole animation's length (--seconds) or as the
time one frame is held (--frame-time); one of the two is required because the
other one falls out of the frame count. GIF only stores delays in hundredths
of a second, so the per-frame delay is rounded against a running total instead
of on its own — that keeps the animation's total length right even when the
requested frame time is not a whole centisecond.

Resolution defaults to the size of the largest input image (by pixel count),
which is what you want when the folder is one set of screenshots with the odd
smaller one mixed in. Smaller frames are scaled up to it, not padded at native
size, so nothing appears to jump between frames.

Usage:
    python tools/imgs2gif.py screenshots -t 12            # 12 s all up
    python tools/imgs2gif.py screenshots -f 0.75          # 0.75 s a frame
    python tools/imgs2gif.py screenshots -t 12 -r 800x480
    python tools/imgs2gif.py screenshots -t 12 -r 800     # height follows
    python tools/imgs2gif.py screenshots -t 12 --fit cover -o docs/demo.gif
"""

import argparse
import os
import re
import sys

try:
    from PIL import Image, ImageColor, ImageOps
except ImportError:
    sys.exit("this needs Pillow: pip install Pillow")

EXTS = (".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".tif", ".tiff",
        ".ppm", ".pgm")

# Below two centiseconds most viewers (browsers included) quietly substitute
# ten, so an animation asking for less runs far slower than requested.
MIN_SANE_CS = 2


def natural_key(name):
    """Sort key that reads runs of digits as numbers: img9 before img10."""
    return [int(t) if t.isdigit() else t.lower()
            for t in re.split(r"(\d+)", name)]


def collect(folder, skip):
    """Every image directly in `folder`, natural order. Not recursive — a
    folder of frames is flat, and walking would sweep up thumbnails."""
    if not os.path.isdir(folder):
        sys.exit("%s: not a folder" % folder)
    skip = os.path.abspath(skip) if skip else None
    names = [n for n in os.listdir(folder) if n.lower().endswith(EXTS)]
    names.sort(key=natural_key)
    paths = []
    for n in names:
        p = os.path.join(folder, n)
        if not os.path.isfile(p):
            continue
        if skip and os.path.abspath(p) == skip:
            continue  # re-running in place must not eat its own output
        paths.append(p)
    if not paths:
        sys.exit("%s: no images found" % folder)
    return paths


def load(path):
    """Open as RGB, honouring EXIF rotation. An animated input contributes
    only the frame it opens on, which is its first."""
    im = ImageOps.exif_transpose(Image.open(path)) or Image.open(path)
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
    return im


def flatten(im, bg):
    """Composite anything with alpha onto the background: GIF's one-bit
    transparency would otherwise turn soft edges into ragged fringes."""
    if im.mode == "RGBA":
        plate = Image.new("RGB", im.size, bg)
        plate.paste(im, mask=im.getchannel("A"))
        return plate
    return im.convert("RGB")


def parse_size(spec, base):
    """'800x480', or '800' / '800x' / 'x480' to fix one axis and let the other
    follow the largest image's aspect ratio."""
    m = re.fullmatch(r"\s*(\d*)\s*[xX*]?\s*(\d*)\s*", spec)
    if not m or not (m.group(1) or m.group(2)):
        sys.exit("--resolution: expected WxH, W or xH, got %r" % spec)
    w = int(m.group(1)) if m.group(1) else 0
    h = int(m.group(2)) if m.group(2) else 0
    bw, bh = base
    if not w:
        w = max(1, round(bw * h / bh))
    if not h:
        h = max(1, round(bh * w / bw))
    return w, h


def resize(im, size, fit, bg):
    if im.size == size:
        return im
    if fit == "stretch":
        return im.resize(size, Image.LANCZOS)
    if fit == "cover":
        return ImageOps.fit(im, size, Image.LANCZOS)
    # contain: scale whole, pad the leftover with the background colour
    scale = min(size[0] / im.width, size[1] / im.height)
    inner = (max(1, round(im.width * scale)), max(1, round(im.height * scale)))
    plate = Image.new("RGB", size, bg)
    plate.paste(im.resize(inner, Image.LANCZOS),
                ((size[0] - inner[0]) // 2, (size[1] - inner[1]) // 2))
    return plate


def delays_cs(total, n):
    """Split `total` seconds over n frames in centiseconds, carrying the
    rounding error forward so the frames still add up to `total`."""
    want = total * 100.0
    out, emitted = [], 0
    for i in range(1, n + 1):
        d = int(round(want * i / n)) - emitted
        d = max(1, d)  # a zero delay means "as fast as possible", not "skip"
        emitted += d
        out.append(d)
    return out


def global_palette(frames, colors):
    """One palette for the whole animation, derived from thumbnails of every
    frame stacked into a single image. Costs some colour fidelity, buys a
    steady look — per-frame palettes make flat backgrounds shimmer."""
    thumbs = []
    for f in frames:
        t = f.copy()
        t.thumbnail((128, 128), Image.LANCZOS)
        thumbs.append(t)
    w = max(t.width for t in thumbs)
    strip = Image.new("RGB", (w, sum(t.height for t in thumbs)))
    y = 0
    for t in thumbs:
        strip.paste(t, (0, y))
        y += t.height
    return strip.quantize(colors=colors, method=Image.Quantize.MEDIANCUT)


def main():
    p = argparse.ArgumentParser(
        description="Make an animated GIF from a folder of images.")
    p.add_argument("folder", help="folder holding the frames")
    t = p.add_mutually_exclusive_group(required=True)
    t.add_argument("-t", "--seconds", type=float,
                   help="length of the whole animation, in seconds")
    t.add_argument("-f", "--frame-time", type=float,
                   help="how long one frame is held, in seconds")
    p.add_argument("-r", "--resolution",
                   help="WxH (or W / xH); default: the largest image's size")
    p.add_argument("-o", "--out",
                   help="output path (default: <folder>.gif alongside it)")
    p.add_argument("--fit", choices=("contain", "cover", "stretch"),
                   default="contain",
                   help="how off-aspect frames meet the size (default contain,"
                        " which pads with --bg)")
    p.add_argument("--bg", default="black",
                   help="padding / alpha backdrop colour (default black)")
    p.add_argument("--colors", type=int, default=256,
                   help="palette size, 2..256 (default 256)")
    p.add_argument("--per-frame-palette", action="store_true",
                   help="give each frame its own palette: truer colour, but "
                        "flat areas can shimmer between frames")
    p.add_argument("--no-dither", action="store_true",
                   help="no dithering; flatter but banded, and smaller")
    p.add_argument("--loop", type=int, default=0,
                   help="times to repeat, 0 = forever (default)")
    p.add_argument("--reverse", action="store_true", help="play last to first")
    a = p.parse_args()

    if not 2 <= a.colors <= 256:
        sys.exit("--colors must be between 2 and 256")
    if (a.seconds is not None and a.seconds <= 0) or \
       (a.frame_time is not None and a.frame_time <= 0):
        sys.exit("the time must be positive")
    try:
        bg = ImageColor.getrgb(a.bg)[:3]
    except ValueError:
        sys.exit("--bg: %r is not a colour Pillow knows" % a.bg)

    out = a.out or (os.path.normpath(a.folder) + ".gif")
    paths = collect(a.folder, out)
    if a.reverse:
        paths.reverse()

    print("%d frames from %s" % (len(paths), a.folder))
    frames = []
    for path in paths:
        try:
            frames.append(flatten(load(path), bg))
        except OSError as e:
            print("  skipping %s: %s" % (os.path.basename(path), e))
    if not frames:
        sys.exit("nothing decodable in %s" % a.folder)

    biggest = max(frames, key=lambda im: im.width * im.height).size
    size = parse_size(a.resolution, biggest) if a.resolution else biggest
    print("  size %dx%d (%s)" %
          (size[0], size[1], "given" if a.resolution else "largest image"))
    frames = [resize(im, size, a.fit, bg) for im in frames]

    total = a.seconds if a.seconds is not None else a.frame_time * len(frames)
    cs = delays_cs(total, len(frames))
    if min(cs) < MIN_SANE_CS:
        print("  warning: %.3f s a frame is below the %.2f s most viewers "
              "honour; they will play it slower" % (min(cs) / 100.0,
                                                    MIN_SANE_CS / 100.0))

    dither = Image.Dither.NONE if a.no_dither else Image.Dither.FLOYDSTEINBERG
    if a.per_frame_palette:
        frames = [im.convert("P", palette=Image.Palette.ADAPTIVE,
                             colors=a.colors, dither=dither) for im in frames]
    else:
        pal = global_palette(frames, a.colors)
        frames = [im.quantize(palette=pal, dither=dither) for im in frames]

    outdir = os.path.dirname(os.path.abspath(out))
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    frames[0].save(out, save_all=True, append_images=frames[1:],
                   duration=[d * 10 for d in cs], loop=a.loop, optimize=True,
                   disposal=1)  # leave each frame up; they all cover the canvas
    print("%s: %.2f s, %.3f s a frame%s, %.1f MB" %
          (out, sum(cs) / 100.0, sum(cs) / 100.0 / len(cs),
           "" if len(set(cs)) == 1 else " on average",
           os.path.getsize(out) / 1048576.0))


if __name__ == "__main__":
    main()
