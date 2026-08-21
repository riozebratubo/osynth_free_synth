#!/usr/bin/env python3
"""S37h - pull whatever is readable out of the board documents in schematics/.

The Guition JC-ESP32P4-M3-DEV spec PDF is mostly marketing and its two useful
pages ("Interface Description", "Product Size") are images. The Altium libraries
are module-level only: the SCHLIB carries the module symbol's pin names and the
PcbLib its footprint, and **neither contains a dev-board net list** — no ES8311,
no I2S nets, nothing about the audio block. Recorded so nobody re-opens them
expecting the audio sheet.

What they did establish:

  - The board is a Guition JC-ESP32P4-M3-DEV, not the Waveshare P4-NANO-class
    tablet every note assumed. `SOURCELIBRARYNAME=ESP32P4_KSDIY_V3.SCHLIB`.
  - The module brings out GPIO0-13, 20-23 and 26-54, so GPIO48 is reachable —
    which is what the microphone's real data pin turned out to be (S37h).

Outputs, all under schematics/:
  _spec_text.txt        page-by-page text of the PDF
  _schlib_strings.txt   printable strings from the SCHLIB (module pin names)
  _pcblib_strings.txt   printable strings from the PcbLib
  _extracted/           every embedded image in the PDF

Needs `pypdf`. Kept per the project's artifacts rule.
"""
import io
import os
import re

SCH = 'schematics'
PDF = os.path.join(SCH, 'JC-ESP32P4-M3-DEV Specifications-EN.pdf')


def dump_pdf_text():
    import pypdf
    r = pypdf.PdfReader(PDF)
    with io.open(os.path.join(SCH, '_spec_text.txt'), 'w', encoding='utf-8') as f:
        for i, p in enumerate(r.pages):
            t = (p.extract_text() or '').strip()
            f.write(u'\n=== page %d (%d chars) ===\n' % (i + 1, len(t)))
            f.write(t + u'\n')
    print('wrote _spec_text.txt (%d pages)' % len(r.pages))


def dump_pdf_images():
    import pypdf
    r = pypdf.PdfReader(PDF)
    out = os.path.join(SCH, '_extracted')
    if not os.path.isdir(out):
        os.makedirs(out)
    n = 0
    for i, p in enumerate(r.pages):
        try:
            imgs = list(p.images)
        except Exception as exc:
            print('page %d: %s' % (i + 1, exc))
            continue
        for j, im in enumerate(imgs):
            with open(os.path.join(out, 'p%d_%d_%s' % (i + 1, j, im.name)), 'wb') as f:
                f.write(im.data)
            n += 1
    print('wrote %d images to _extracted/' % n)


def dump_strings(src, dst):
    data = open(os.path.join(SCH, src), 'rb').read()
    seen, out = set(), []
    for run in re.findall(rb'[\x20-\x7e]{4,}', data):
        s = run.decode('ascii', 'replace')
        if s not in seen:
            seen.add(s)
            out.append(s)
    with io.open(os.path.join(SCH, dst), 'w', encoding='utf-8') as f:
        f.write(u'\n'.join(out))
    print('wrote %s (%d unique strings)' % (dst, len(out)))


if __name__ == '__main__':
    dump_pdf_text()
    dump_pdf_images()
    dump_strings('JC-ESP32P4-M3.SCHLIB', '_schlib_strings.txt')
    dump_strings('JC-ESP32P4-M3.PcbLib', '_pcblib_strings.txt')
    gpios = re.findall(r'GPIO\d+',
                       io.open(os.path.join(SCH, '_schlib_strings.txt'),
                               encoding='utf-8').read())
    uniq = sorted(set(gpios), key=lambda s: int(s[4:]))
    print('module exposes: %s' % ' '.join(uniq))
