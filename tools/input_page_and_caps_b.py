#!/usr/bin/env python3
"""Follow-up to tools/input_page_and_caps.py:

  * migrateScreenOrder() records that it ran BEFORE it moves anything. The
    other order shifts an index twice if the marker write is the one that
    fails.
  * the note about the removed osynth page moves out of the middle of the
    screens array literal and into the block comment above it, where the next
    person appending an entry will actually read it.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

changes = []


def sub(path, old, new):
    changes.append((os.path.join(ROOT, path), old, new))


sub("app_osyntho/src/business/settings.cpp",
    """void Settings::migrateScreenOrder() {
  if (settingsCache.value("screen_order_rev") == "1") return;

  const auto remap = [](const QString& value) {""",

    """void Settings::migrateScreenOrder() {
  if (settingsCache.value("screen_order_rev") == "1") return;
  // Recorded before anything moves, and the migration abandoned if it cannot
  // be: a remap that ran but failed to say so would run again on the next
  // launch and shift the same index a second time. Failing here instead leaves
  // every stored value exactly as it was, which is merely wrong rather than
  // progressively wrong.
  if (saveSetting(QStringLiteral("screen_order_rev"), QStringLiteral("1")) == 0) return;

  const auto remap = [](const QString& value) {""")

sub("app_osyntho/src/business/settings.cpp",
    """  for (const QString& key :
       {QStringLiteral("startup_screen"), QStringLiteral("last_swipeview_index")}) {
    const QString moved = remap(settingsCache.value(key));
    if (moved != settingsCache.value(key)) saveSetting(key, moved);
  }
  saveSetting(QStringLiteral("screen_order_rev"), QStringLiteral("1"));
}""",

    """  for (const QString& key :
       {QStringLiteral("startup_screen"), QStringLiteral("last_swipeview_index")}) {
    const QString moved = remap(settingsCache.value(key));
    if (moved != settingsCache.value(key)) saveSetting(key, moved);
  }
}""")

sub("app_osyntho/qml/UI.qml",
    """    // list *is* its SwipeView index, so the two must stay in the same order as
    // the pages declared in Main.qml. Strings are kept in English here and
    // translated where they are shown, as everywhere else.""",

    """    // list *is* its SwipeView index, so the two must stay in the same order as
    // the pages declared in Main.qml. Strings are kept in English here and
    // translated where they are shown, as everywhere else.
    //
    // Inserting or removing an entry moves every index after it, and two
    // settings store one — the startup screen and the page the last run was
    // left on. Bump screen_order_rev and extend Settings::migrateScreenOrder()
    // when you change this list, or people's stored choices quietly slide one
    // page over. (That is what the last edit did: an Input page went in before
    // FX and the osynth page came off the end. It held three things and
    // repeated two of them — master volume was already on the toolbar and its
    // `in.` group was already on FX — so the analogue output level and the USB
    // card moved to Home and the input took a page of its own.)""")

sub("app_osyntho/qml/UI.qml",
    """        { label: "Loc. Pre", name: "Local presets",  icon: "\\uf02d" }  // book
        // There was an osynth (Dev) page here holding the synth's own
        // persisted settings (S35). It held three things and repeated two of
        // them: master volume was already on the toolbar and its `in.` group
        // was already on FX. What was genuinely only there — the analogue
        // output level and the USB port card — is on Home now, and the input
        // has the page above. Removing it moved every index from FX up, so
        // Settings::migrateScreenOrder() remaps the two settings that store
        // one.
    ]""",

    """        { label: "Loc. Pre", name: "Local presets",  icon: "\\uf02d" }  // book
    ]""")


files = {}
for path, old, new in changes:
    if path not in files:
        with io.open(path, encoding="utf-8") as fh:
            files[path] = fh.read()

failed = []
for path, old, new in changes:
    text = files[path]
    if text.count(old) != 1:
        failed.append((path, text.count(old), old.splitlines()[0][:72]))
        continue
    files[path] = text.replace(old, new, 1)

if failed:
    for path, n, head in failed:
        print("ANCHOR x%d in %s: %s" % (n, os.path.relpath(path, ROOT), head))
    sys.exit(1)

for path, text in sorted(files.items()):
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("patched", os.path.relpath(path, ROOT))
