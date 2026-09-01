#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Give the standalone (OSYNTHO_EMBEDDED=ON) build its own application identity.

Applied 2026-09-01. Idempotent: every replacement asserts its anchor is present,
so a second run fails loudly instead of doubling the edits.

The remaining half of this change was made directly in the files it touches
(app_osyntho/CMakeLists.txt, main.cpp, src/app.{h,cpp}, qml/Main.qml, the two
Info.plists, the Android manifest template, deploy_linux.sh,
deploy_macos_multi.sh). This script only carries the deploy_android.bat edits,
which are the ones a here-doc mangles: cmd paths are full of backslashes.
"""

import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BAT = os.path.join(ROOT, "deploy_android.bat")

NL = "\r\n"


def sub(text, old, new):
    assert old in text, "anchor not found:\n" + old
    return text.replace(old, new, 1)


def main():
    s = io.open(BAT, encoding="utf-8", newline="").read()

    # -- Variant identity, applied once the arguments have been parsed. --------
    s = sub(
        s,
        ":args_done" + NL + NL
        + 'if not defined ABIS ( call :die "--abis was given an empty list" & exit /b 1 )' + NL,
        ":args_done" + NL + NL
        + 'if not defined ABIS ( call :die "--abis was given an empty list" & exit /b 1 )' + NL
        + NL
        + ":: -- Variant identity -------------------------------------------------------" + NL
        + ":: Mirrors the APP_ID / APP_DISPLAY_NAME derivation in" + NL
        + ":: app_osyntho" + chr(92) + "CMakeLists.txt. Kept in step with it by hand; the values are" + NL
        + ":: printed below, so a drift shows up before the build rather than as an" + NL
        + ":: install that refuses with a conflicting-provider error." + NL
        + 'if /i "!EMBEDDED!"=="ON" (' + NL
        + '    set "APP_ID=org.osynth.osyntho.standalone"' + NL
        + '    set "APP_DISPLAY_NAME=Osyntho Standalone"' + NL
        + '    set "APP_FILE_BASE=Osyntho-Standalone"' + NL
        + '    set "VARIANT=standalone (embedded synth engine)"' + NL
        + ") else (" + NL
        + '    set "VARIANT=controller (BLE)"' + NL
        + ")" + NL,
    )

    # -- The variant is part of the default build dir. ------------------------
    build_dir_line = (
        'if not defined BUILD_DIR  set "BUILD_DIR=%APP_SRC_DIR%'
        + chr(92) + "build" + chr(92) + 'android-!ABI_TAG!-!BUILD_TYPE!"' + NL
    )
    s = sub(
        s,
        build_dir_line,
        ":: The variant is part of the default path. The two configurations differ in" + NL
        + ":: far more than a define -- one links Qt Bluetooth, the other compiles the" + NL
        + ":: whole synth engine -- so sharing a cache between them buys nothing and" + NL
        + ":: costs a package name silently left over from the previous run." + NL
        + 'set "VARIANT_TAG="' + NL
        + 'if /i "!EMBEDDED!"=="ON" set "VARIANT_TAG=-standalone"' + NL
        + 'if not defined BUILD_DIR  set "BUILD_DIR=%APP_SRC_DIR%'
        + chr(92) + "build" + chr(92) + 'android-!ABI_TAG!-!BUILD_TYPE!!VARIANT_TAG!"' + NL,
    )

    # -- Say which one is being built. ----------------------------------------
    s = sub(
        s,
        'call :info "Package      : !APP_ID!"' + NL,
        'call :info "Variant      : !VARIANT!"' + NL
        + 'call :info "Package      : !APP_ID!"' + NL,
    )

    # -- Hand the choice to CMake (and to the --check preview, which prints
    #    EXTRA_ARGS). -----------------------------------------------------------
    s = sub(
        s,
        'if defined SDK_BUILD_TOOLS  set "EXTRA_ARGS=!EXTRA_ARGS! '
        + '"-DQT_ANDROID_SDK_BUILD_TOOLS_REVISION=!SDK_BUILD_TOOLS!""' + NL,
        'if defined SDK_BUILD_TOOLS  set "EXTRA_ARGS=!EXTRA_ARGS! '
        + '"-DQT_ANDROID_SDK_BUILD_TOOLS_REVISION=!SDK_BUILD_TOOLS!""' + NL
        + ":: Always passed, never left to the CMake default: an existing build dir would" + NL
        + ":: otherwise keep whatever OSYNTHO_EMBEDDED its cache already holds, and the" + NL
        + ":: package name reported above would be a guess." + NL
        + 'set "EXTRA_ARGS=!EXTRA_ARGS! "-DOSYNTHO_EMBEDDED=!EMBEDDED!""' + NL,
    )

    # -- Usage text. ----------------------------------------------------------
    s = sub(
        s,
        "echo   --aab             Also build the Play Store .aab bundle" + NL,
        "echo   --aab             Also build the Play Store .aab bundle" + NL
        + "echo   --standalone      Embedded synth engine, package org.osynth.osyntho.standalone" + NL
        + "echo   --controller      BLE controller, package org.osynth.osyntho [DEFAULT]" + NL,
    )

    io.open(BAT, "w", encoding="utf-8", newline="").write(s)
    print("patched", BAT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
