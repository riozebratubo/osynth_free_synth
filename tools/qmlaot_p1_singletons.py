#!/usr/bin/env python3
"""Phase 1 of the QML AOT work: context properties -> QML singleton types.

qmlcachegen can only compile a binding to C++ when it can resolve every name in
it. Names injected with QQmlContext::setContextProperty are invisible to it --
they only exist once an engine is running -- so every binding touching App,
Synth, t or BluetoothManager fell back to interpreted byte code. That was 672 of
the 1188 fallbacks in the baseline (tools/out/aot_baseline.json).

This script does the mechanical half of the conversion:
  * declarative registration macros on App, SynthController, Translator, Theme
  * drops the four setContextProperty calls and the dead qmlRegisterType
  * renames the `t` context property to `Tr` across the QML (QML type names
    must start with an upper-case letter, so `t` could not survive as-is)
  * adds `import org.osynth.osyntho` to QML files that now need it

Synth and BluetoothManager are NOT registered here. Both live in the
hand-written src/qmlforeign.h, as QML_FOREIGN wrappers, because
QQmlPrivate::singletonConstructionMode() tests default-constructibility BEFORE
it looks for a create() factory:

    FactoryWrapper (foreign type with create())  ->  Constructor (T is
    default-constructible)  ->  Factory (T::create())

SynthController is default-constructible -- App owns one as a member -- so
registering it directly put it in Constructor mode, and QML's `Synth` became a
second, freshly built controller with no BLE link. A foreign wrapper is the
only way to force the factory. IBluetoothManager is abstract and must not name
a backend, which is the other reason to register from outside.

Idempotent: running it twice changes nothing. Pass --check to report without
writing.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app_osyntho")
SRC = os.path.join(APP, "src")
QML = os.path.join(APP, "qml")
MODULE_URI = "org.osynth.osyntho"

ARGS = None
changes = []


def edit(path, transform):
    with open(path, encoding="utf-8") as handle:
        before = handle.read()
    after = transform(before)
    if after == before:
        return False
    changes.append(os.path.relpath(path, ROOT).replace("\\", "/"))
    if not ARGS.check:
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(after)
    return True


def add_include(text, include):
    """Insert an #include after the last existing one, if not already there."""
    if include in text:
        return text
    matches = list(re.finditer(r"^#include .*$", text, re.M))
    if not matches:
        sys.exit("no #include block to extend")
    last = matches[-1]
    return text[: last.end()] + "\n" + include + text[last.end():]


def add_macros(text, class_decl, macros):
    """Insert QML registration macros right after a class's Q_OBJECT/Q_GADGET."""
    if macros[0] in text:
        return text
    if class_decl not in text:
        sys.exit("class declaration not found: " + class_decl)
    index = text.index(class_decl)
    tail = text[index:]
    marker = re.search(r"^([ \t]*)(Q_OBJECT|Q_GADGET)[ \t]*$", tail, re.M)
    if not marker:
        sys.exit("no Q_OBJECT/Q_GADGET after " + class_decl)
    indent = marker.group(1)
    block = "".join("\n" + indent + macro for macro in macros)
    at = index + marker.end()
    return text[:at] + block + text[at:]


# ---------------------------------------------------------------- C++ headers

def patch_theme(text):
    text = add_include(text, "#include <QtQml/qqmlregistration.h>")
    # Anonymous: QML never declares a Theme of its own, it only reads
    # App.theme.<prop>. Registering it is what lets those reads resolve to the
    # gadget's properties instead of dying in a QVariant.
    return add_macros(text, "class Theme {", ["QML_ANONYMOUS"])


def patch_app(text):
    text = add_include(text, "#include <QtQml/qqmlregistration.h>")
    text = add_macros(text, "class App : public QObject {",
                      ["QML_ELEMENT", "QML_SINGLETON"])
    if "static App* create(" in text:
        return text
    anchor = "  static App& instance();\n"
    creator = anchor + (
        "\n"
        "  // QML singleton factory. The engine never owns App -- instance() does --\n"
        "  // so ownership is pinned to C++ before the pointer is handed over.\n"
        "  static App* create(QQmlEngine*, QJSEngine*);\n"
    )
    return text.replace(anchor, creator, 1)


def patch_app_impl(text):
    if "App* App::create(" in text:
        return text
    text = add_include(text, "#include <QtQml/qqmlengine.h>")
    return text.rstrip("\n") + (
        "\n\n"
        "App* App::create(QQmlEngine*, QJSEngine*) {\n"
        "  App* app = &App::instance();\n"
        "  QJSEngine::setObjectOwnership(app, QJSEngine::CppOwnership);\n"
        "  return app;\n"
        "}\n"
    )


def patch_translator(text):
    text = add_include(text, "#include <QtQml/qqmlregistration.h>")
    # `t` cannot be a QML type name (they must start upper-case), so the
    # singleton is Tr and the QML call sites were renamed to match.
    text = add_macros(text, "class Translator : public QObject {",
                      ["QML_NAMED_ELEMENT(Tr)", "QML_SINGLETON"])
    if "static Translator* create(" in text:
        return text
    anchor = "  static Translator& instance();\n"
    creator = anchor + "\n  static Translator* create(QQmlEngine*, QJSEngine*);\n"
    return text.replace(anchor, creator, 1)


def patch_translator_impl(text):
    if "Translator* Translator::create(" in text:
        return text
    text = add_include(text, "#include <QtQml/qqmlengine.h>")
    return text.rstrip("\n") + (
        "\n\n"
        "Translator* Translator::create(QQmlEngine*, QJSEngine*) {\n"
        "  Translator* translator = &Translator::instance();\n"
        "  QJSEngine::setObjectOwnership(translator, QJSEngine::CppOwnership);\n"
        "  return translator;\n"
        "}\n"
    )


# ------------------------------------------------------------------- main.cpp

CONTEXT_NOTE = (
    "  // App, Synth, Tr and BluetoothManager are declared QML singletons of the\n"
    "  // org.osynth.osyntho module now. They used to be context properties, which\n"
    "  // no compiler can see through: every binding that touched one fell back to\n"
    "  // interpreted byte code. See tools/qml_aot_score.py for the coverage.\n"
)


def patch_main(text):
    text = re.sub(
        r"[ \t]*// qml inserted instances\n"
        r"(?:[ \t]*engine\.rootContext\(\)->setContextProperty\([^;]*;\n)+",
        CONTEXT_NOTE, text)
    # Dead: nothing imports org.osynth.main, and SynthController is this
    # module's Synth singleton now.
    text = re.sub(
        r"[ \t]*// qml types\. SynthController is also exposed as the `Synth` context property\n"
        r"[ \t]*// below; the registration lets QML import its enums if needed\.\n"
        r"[ \t]*qmlRegisterType<SynthController>\([^;]*;\n", "", text)
    return text


# ----------------------------------------------------------------------- QML

def rename_translator(text):
    # t.t(...), t.ts(...), t.setActiveLanguage(...). The lookbehind keeps the
    # rename off `.t.`, `foo_t.` and anything that is part of a longer name.
    return re.sub(r"(?<![\w.$])t\.(t|ts|setActiveLanguage)\b", r"Tr.\1", text)


SINGLETON_NAMES = ("App", "Synth", "Tr", "BluetoothManager")
_SINGLETON_RE = re.compile(
    r"(?<![\w.$])(" + "|".join(SINGLETON_NAMES) + r")[ \t]*\.")


def strip_noise(text):
    """Blank out comments and string literals before scanning for identifiers."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    text = re.sub(r"'(?:[^'\\\n]|\\.)*'", "''", text)
    return text


def add_module_import(text):
    if re.search(r"^import[ \t]+" + re.escape(MODULE_URI) + r"[ \t]*$", text, re.M):
        return text
    if not _SINGLETON_RE.search(strip_noise(text)):
        return text
    imports = list(re.finditer(r"^import .*$", text, re.M))
    if not imports:
        sys.exit("no import block to extend")
    last = imports[-1]
    return text[: last.end()] + "\nimport " + MODULE_URI + text[last.end():]


def main():
    global ARGS
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="report without writing")
    ARGS = parser.parse_args()

    edit(os.path.join(SRC, "theme.h"), patch_theme)
    edit(os.path.join(SRC, "app.h"), patch_app)
    edit(os.path.join(SRC, "app.cpp"), patch_app_impl)
    edit(os.path.join(SRC, "translator.h"), patch_translator)
    edit(os.path.join(SRC, "translator.cpp"), patch_translator_impl)
    edit(os.path.join(APP, "main.cpp"), patch_main)

    for name in sorted(os.listdir(QML)):
        if name.endswith(".qml"):
            edit(os.path.join(QML, name),
                 lambda text: add_module_import(rename_translator(text)))

    verb = "would change" if ARGS.check else "changed"
    print("{} {} files".format(verb, len(changes)))
    for path in changes:
        print("  " + path)


if __name__ == "__main__":
    main()
