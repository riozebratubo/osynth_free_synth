#!/usr/bin/env python3
"""Extract text from a PDF grouped by position on the page.

pypdf's plain extract_text() walks the content stream in draw order, which for
a schematic means net labels, ref-designators and pin numbers come out in an
order that has nothing to do with what is wired to what. This walks the same
text with a visitor, keeps each fragment's transform-matrix X/Y, and rebuilds
lines by Y — which is enough to read a net label off the same row as the pin it
lands on.

Used to recover the M5Stack Module Audio (M144) netlist from
private_docs/datasheets/M144_sch_moduleaudio_v10.pdf; see the "M5Stack Module
Audio" section of private_docs/HARDWARE.md.

    python tools/pdf_layout_text.py <file.pdf> [--page N] [--tol 6]

Each output line is one row of the page, fragments as `X:text` left to right.
--tol is how many units apart two fragments can be vertically and still count
as the same row; widen it on a sparse drawing, narrow it on dense body text.
"""

import argparse

import pypdf


def rows(page, tol):
    items = []

    def visit(text, cm, tm, font_dict, font_size):
        stripped = text.strip()
        if stripped:
            items.append((round(tm[5]), round(tm[4]), stripped))

    page.extract_text(visitor_text=visit)
    items.sort(key=lambda item: (-item[0], item[1]))

    line, prev_y = [], None
    for y, x, text in items:
        if prev_y is None or abs(y - prev_y) > tol:
            if line:
                yield line
            line, prev_y = [], y
        line.append(f"{x}:{text}")
    if line:
        yield line


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pdf")
    ap.add_argument("--page", type=int, help="1-based page number; default all")
    ap.add_argument("--tol", type=int, default=6, help="row grouping tolerance")
    args = ap.parse_args()

    reader = pypdf.PdfReader(args.pdf)
    pages = [args.page - 1] if args.page else range(len(reader.pages))
    for index in pages:
        print(f"===== page {index + 1} =====")
        for line in rows(reader.pages[index], args.tol):
            print(" | ".join(line))


if __name__ == "__main__":
    main()
