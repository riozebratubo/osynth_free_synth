#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Copy the Windows static-Qt osyntho.exe to <repo>/current/<variant>/.

Applied 2026-09-01, on top of tools/applied_edits/standalone_variant_ids.py.
Adds APP_VARIANT ("standalone" / "controller") to the variant-identity block in
app_osyntho/CMakeLists.txt, and a POST_BUILD copy that fires only for a Windows
build against a statically linked Qt -- the configuration whose .exe is
self-contained and therefore worth dropping outside the build tree.

Idempotent in the strict sense: every replacement asserts its anchor, so a
second run fails rather than doubling the edit.
"""

import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CML = os.path.join(ROOT, "app_osyntho", "CMakeLists.txt")
GITIGNORE = os.path.join(ROOT, ".gitignore")


def sub(text, old, new):
    assert old in text, "anchor not found:\n" + old
    return text.replace(old, new, 1)


def patch_cmakelists():
    s = io.open(CML, encoding="utf-8", newline="").read()
    nl = "\r\n" if "\r\n" in s else "\n"

    # 1. APP_VARIANT alongside APP_ID / APP_DISPLAY_NAME.
    s = sub(
        s,
        "if (OSYNTHO_EMBEDDED)" + nl
        + '  set(APP_ID "${APP_ID_BASE}.standalone")' + nl
        + '  set(APP_DISPLAY_NAME "Osyntho Standalone")' + nl
        + "else()" + nl
        + '  set(APP_ID "${APP_ID_BASE}")' + nl
        + '  set(APP_DISPLAY_NAME "Osyntho")' + nl
        + "endif()" + nl,
        "#" + nl
        + "# APP_VARIANT is the same choice as a bare word, for places that need a path" + nl
        + "# segment rather than a name: the Windows static-Qt drop at the bottom of this" + nl
        + "# file writes to <repo>/current/${APP_VARIANT}/." + nl
        + "if (OSYNTHO_EMBEDDED)" + nl
        + '  set(APP_ID "${APP_ID_BASE}.standalone")' + nl
        + '  set(APP_DISPLAY_NAME "Osyntho Standalone")' + nl
        + '  set(APP_VARIANT "standalone")' + nl
        + "else()" + nl
        + '  set(APP_ID "${APP_ID_BASE}")' + nl
        + '  set(APP_DISPLAY_NAME "Osyntho")' + nl
        + '  set(APP_VARIANT "controller")' + nl
        + "endif()" + nl,
    )

    # 2. The POST_BUILD copy, after the existing qmldir one.
    anchor = (
        "# fix empty qmldir file not found on org.osynth" + nl
        + "add_custom_command(" + nl
        + "    TARGET ${PROJECT_NAME} POST_BUILD" + nl
        + "    COMMAND ${CMAKE_COMMAND} -E copy_if_different" + nl
        + "        ${CMAKE_SOURCE_DIR}/assets/qmldir" + nl
        + "        ${CMAKE_CURRENT_BINARY_DIR}/org/osynth/qmldir" + nl
        + ")" + nl
    )

    block = """
# --- Windows static-Qt drop --------------------------------------------------
# A static Qt is the one configuration whose osyntho.exe stands on its own: no
# Qt DLLs beside it, no windeployqt pass, nothing to assemble before it runs.
# That makes it the only build worth lifting straight out of the build tree, so
# it also lands at <repo>/current/<variant>/ -- a fixed path per variant to run
# from or hand to someone, instead of a build directory whose name encodes the
# kit and the configuration. The build tree keeps its own copy either way; this
# is a copy, not a relocation.
#
# The two variants get their own subdirectory for the same reason they get their
# own app id: both executables are called osyntho.exe, so one directory would
# mean each build silently replacing the other.
#
# Static vs shared is read off Qt6::Core's imported target type. Qt's own
# QT_BUILD_SHARED_LIBS is set by Qt6BuildInternalsExtra.cmake, which an
# application project never loads, so it is not available here -- whereas the
# imported library is STATIC_LIBRARY or SHARED_LIBRARY in every Qt 6 kit.
if (WIN32 AND TARGET Qt6::Core)
  get_target_property(OSYNTHO_QT_CORE_TYPE Qt6::Core TYPE)
  if (OSYNTHO_QT_CORE_TYPE STREQUAL "STATIC_LIBRARY")
    # ../ is the repository root -- the same assumption add_subdirectory() makes
    # for port/host above.
    get_filename_component(OSYNTHO_REPO_DIR "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
    set(OSYNTHO_CURRENT_DIR "${OSYNTHO_REPO_DIR}/current/${APP_VARIANT}")
    message(STATUS
        "osyntho: static Qt kit -- the .exe will also be copied to ${OSYNTHO_CURRENT_DIR}")

    # copy_if_different rather than copy: a rebuild that produced the same bytes
    # leaves the file alone, so a copy someone is running from is not replaced
    # underneath them (Windows refuses that anyway, and the build would fail).
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OSYNTHO_CURRENT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${PROJECT_NAME}>"
                "${OSYNTHO_CURRENT_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMENT "Copying the static build to current/${APP_VARIANT}/"
        VERBATIM)
  else()
    message(STATUS
        "osyntho: shared Qt kit -- not copying to current/${APP_VARIANT} "
        "(the .exe there would need the Qt DLLs beside it)")
  endif()
endif()
"""
    if nl != "\n":
        block = block.replace("\n", nl)

    s = sub(s, anchor, anchor + block)
    io.open(CML, "w", encoding="utf-8", newline="").write(s)
    print("patched", CML)


def patch_gitignore():
    s = io.open(GITIGNORE, encoding="utf-8", newline="").read()
    nl = "\r\n" if "\r\n" in s else "\n"
    addition = (
        nl
        + "# Windows static-Qt builds drop osyntho.exe here (app_osyntho/CMakeLists.txt)."
        + nl
        + "/current/"
        + nl
    )
    assert "/current/" not in s
    if not s.endswith(nl):
        s += nl
    s += addition
    io.open(GITIGNORE, "w", encoding="utf-8", newline="").write(s)
    print("patched", GITIGNORE)


def main():
    patch_cmakelists()
    patch_gitignore()
    return 0


if __name__ == "__main__":
    sys.exit(main())
