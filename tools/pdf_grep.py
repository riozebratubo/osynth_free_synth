#!/usr/bin/env python3
"""Extract/search text from a PDF. Kept for future datasheet work (S31c).

Usage:  python tools/pdf_grep.py <file.pdf> [regex] [-C n]
No regex -> dumps every page with a page header.
"""
import re
import sys

from pypdf import PdfReader

# Datasheets are full of Ω, °, µ and typographic quotes; the Windows console
# default (cp1252) cannot encode them and the traceback hides the answer.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    pattern = sys.argv[2] if len(sys.argv) > 2 else None
    ctx = 2
    if "-C" in sys.argv:
        ctx = int(sys.argv[sys.argv.index("-C") + 1])

    reader = PdfReader(path)
    rx = re.compile(pattern, re.I) if pattern else None
    for pno, page in enumerate(reader.pages, 1):
        text = page.extract_text() or ""
        if rx is None:
            print(f"\n===== page {pno} =====\n{text}")
            continue
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if rx.search(line):
                lo, hi = max(0, i - ctx), min(len(lines), i + ctx + 1)
                print(f"--- p{pno}:{i} ---")
                print("\n".join(lines[lo:hi]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
