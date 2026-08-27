"""Shared file-edit helper for the S45 split-render patch scripts.

The repo has mixed line endings -- components/audio_io/include/audio_io.h is
CRLF, components/synth_core/include/synth_config.h is LF -- so every anchor
match has to happen on normalised text and every write has to restore whatever
the file already used. Doing that once here is the difference between a patch
script that works on one file and one that works on all of them.
"""
import io
import sys


class Editor(object):
    def __init__(self, path, skip_if=None):
        self.path = path
        raw = io.open(path, encoding="utf-8", newline="").read()
        self.crlf = "\r\n" in raw
        self.text = raw.replace("\r\n", "\n")
        self.skip = bool(skip_if) and skip_if in self.text

    def sub(self, old, new, count=1):
        """Replace `old` with `new`, insisting it appears exactly `count` times."""
        if self.skip:
            return
        seen = self.text.count(old)
        if seen != count:
            sys.exit("%s: anchor seen %d times, wanted %d:\n%s"
                     % (self.path, seen, count, old[:200]))
        self.text = self.text.replace(old, new)

    def save(self, what):
        if self.skip:
            print("%s: already patched; nothing to do" % self.path)
            return False
        out = self.text.replace("\n", "\r\n") if self.crlf else self.text
        io.open(self.path, "w", encoding="utf-8", newline="").write(out)
        print("%s: %s" % (self.path, what))
        return True
