#!/usr/bin/env python3
"""Check that the app's hand-edited XML still parses — manifest and plists.

Why this exists: androiddeployqt reports a malformed AndroidManifest.xml as

    Error in .../android-build-osyntho//AndroidManifest.xml:
        Expected '>', but got ' '.

with no line number, twenty seconds into an APK build, and only after the whole
native build has succeeded. The usual cause is a comment: XML forbids "--"
anywhere inside <!-- -->, because the parser reads it as the start of the
closing bracket. Prose written the way the rest of this repo writes it ("the
manifest -- not the property -- decides") is therefore invalid XML, and nothing
between the editor and androiddeployqt says so.

The manifest is a CMake template (@APP_ID@, @APP_DISPLAY_NAME@), so it is
checked as it will be *generated*, once per build variant. The plists are
templates too, but only in ways that leave them well formed as they sit.

Kept per the project's intermediary-artifacts policy. Run it after editing any
of these files:

    python tools/check_app_xml.py
"""
import pathlib
import sys
import xml.parsers.expat

REPO = pathlib.Path(__file__).resolve().parent.parent
APP = REPO / "app_osyntho"

# The manifest's placeholders and the values each variant gives them. Mirrors
# the APP_ID / APP_DISPLAY_NAME derivation in app_osyntho/CMakeLists.txt.
MANIFEST = APP / "assets" / "android-build" / "AndroidManifest.xml.in"
VARIANTS = {
    "controller": {
        "@APP_ID@": "org.osynth.osyntho",
        "@APP_DISPLAY_NAME@": "Osyntho",
        # Empty for the controller: no engine, so no capture device, so no
        # microphone to ask for. The element below is markup rather than text,
        # which is the reason this placeholder is worth substituting here at
        # all - an unsubstituted one would sit in a text node and parse.
        "@ANDROID_EXTRA_PERMISSIONS@": "",
    },
    "standalone": {
        "@APP_ID@": "org.osynth.osyntho.standalone",
        "@APP_DISPLAY_NAME@": "Osyntho Standalone",
        "@ANDROID_EXTRA_PERMISSIONS@":
            '<uses-permission android:name="android.permission.RECORD_AUDIO" />',
    },
}

# Checked as written. CMake's @MACOSX_BUNDLE_*@ placeholders sit in text nodes,
# which is legal XML whether or not they have been substituted.
PLAIN = [
    APP / "assets" / "macos" / "Info.plist",
    APP / "assets" / "ios" / "Info.plist",
]


def parse(text, label):
    """True if `text` is well-formed XML; prints the failure otherwise."""
    parser = xml.parsers.expat.ParserCreate()
    try:
        parser.Parse(text.encode("utf-8"), True)
    except xml.parsers.expat.ExpatError as exc:
        print("  FAIL %s" % label)
        print("       line %d, column %d: %s"
              % (exc.lineno, exc.offset, xml.parsers.expat.ErrorString(exc.code)))
        line = text.splitlines()[exc.lineno - 1] if exc.lineno <= len(text.splitlines()) else ""
        print("       %s" % line.strip())
        if "--" in line:
            print('       hint: "--" is illegal inside an XML comment.')
        return False
    print("  ok   %s" % label)
    return True


def main():
    ok = True

    if not MANIFEST.is_file():
        print("  FAIL %s is missing" % MANIFEST)
        return 1
    src = MANIFEST.read_text(encoding="utf-8")
    for variant, values in VARIANTS.items():
        text = src
        for placeholder, value in values.items():
            text = text.replace(placeholder, value)
        left = [p for p in values if p in text]
        if left:
            print("  FAIL %s: placeholders not substituted: %s" % (MANIFEST.name, left))
            ok = False
            continue
        ok &= parse(text, "%s  [%s]" % (MANIFEST.name, variant))

    for path in PLAIN:
        if not path.is_file():
            print("  FAIL %s is missing" % path)
            ok = False
            continue
        ok &= parse(path.read_text(encoding="utf-8"), path.name)

    print("all well formed" if ok else "MALFORMED XML - see above")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
