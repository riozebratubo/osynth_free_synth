#!/usr/bin/env python3
"""Placement fixups for tools/qmlaot_p1_singletons.py.

That script appends each new #include after the *last* one in the file, which
is fine for flat headers and wrong for these four: app.cpp ends its include
list inside `#ifdef Q_OS_ANDROID`, and theme.h ends its in the C++
standard-library group. This moves them into the right group and
adds the QQmlEngine / QJSEngine forward declarations that the create() factory
signatures need -- qqmlregistration.h declares neither.

Kept as a separate script rather than folded into the phase 1 one because the
phase 1 edits are generic and these are per-file facts about this tree.

Idempotent. --check reports without writing.
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "app_osyntho", "src")
MAIN = os.path.join(ROOT, "app_osyntho", "main.cpp")

ARGS = None
changes = []

# Qt spells these inside QT_BEGIN_NAMESPACE, which is a no-op unless Qt was
# built with -qtnamespace. Declaring them the same way keeps both builds happy.
FORWARD_DECLS = (
    "QT_BEGIN_NAMESPACE\n"
    "class QQmlEngine;\n"
    "class QJSEngine;\n"
    "QT_END_NAMESPACE\n"
)


def edit(path, pairs, required=True):
    """Apply (old, new) replacements; each `old` must appear exactly once."""
    with open(path, encoding="utf-8") as handle:
        before = handle.read()
    after = before
    for old, new in pairs:
        # Both directions appear here, and each needs its own "already applied"
        # test:
        #   deletion  (new is a prefix of old) -- applied once `old` is gone,
        #             but `new` is present either way.
        #   insertion (old is a prefix of new) -- applied once `new` is there,
        #             but `old` is still present either way, so testing `old`
        #             would re-insert on every run.
        if len(new) > len(old) and new in after:
            continue
        count = after.count(old)
        if count == 1:
            after = after.replace(old, new, 1)
            continue
        if count == 0 and (not required or new in after):
            continue
        sys.exit("{}: expected 1 occurrence of {!r}, found {}".format(
            os.path.basename(path), old[:60], count))
    if after == before:
        return
    changes.append(os.path.relpath(path, ROOT).replace("\\", "/"))
    if not ARGS.check:
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(after)


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()

    # app.cpp: the include was appended into the Q_OS_ANDROID block, where the
    # non-Android builds would never see it.
    edit(os.path.join(SRC, "app.cpp"), [
        ("#include <QtCore/qjniobject.h>\n#include <QtQml/qqmlengine.h>\n",
         "#include <QtCore/qjniobject.h>\n"),
        ("#include <QVariant>\n",
         "#include <QVariant>\n#include <QtQml/qqmlengine.h>\n"),
    ])

    # theme.h: Qt header had been appended after <array>.
    edit(os.path.join(SRC, "theme.h"), [
        ("#include <array>\n#include <QtQml/qqmlregistration.h>\n",
         "#include <array>\n"),
        ("#include <QString>\n",
         "#include <QString>\n#include <QtQml/qqmlregistration.h>\n"),
    ])

    edit(os.path.join(SRC, "translator.h"), [
        ("#include <QtQml/qqmlregistration.h>\n\n",
         "#include <QtQml/qqmlregistration.h>\n\n" + FORWARD_DECLS + "\n"),
    ])

    # main.cpp: the removed qmlRegisterType left a stranded comment and a
    # double blank line behind it.
    edit(MAIN, [
        ("  // metatypes\n\n\n\n", "  // metatypes\n\n"),
    ], required=False)

    verb = "would change" if ARGS.check else "changed"
    print("{} {} files".format(verb, len(changes)))
    for path in changes:
        print("  " + path)


if __name__ == "__main__":
    main()
