#!/bin/bash
# deploy_linux.sh — Package the Osyntho companion app into a portable AppImage.
# Equivalent to windeployqt for the Linux build.
#
# Usage:   ./deploy_linux.sh <build_dir> [output_dir]
# Example: ./deploy_linux.sh ~/build/osyntho-linux-release ~/dist
#
# <build_dir> is the CMake binary dir of app_osyntho/ (the Qt app), NOT of the
# repository root — the root CMakeLists.txt is the ESP-IDF firmware project and
# has nothing to do with this script.
#
# On first run, downloads linuxdeploy + linuxdeploy-plugin-qt into the project
# root and reuses them on subsequent runs.
#
# Environment overrides:
#   BUNDLE_MULTIMEDIA=1   also bundle the QtMultimedia backend plugin + FFmpeg
#                         libs (see the multimedia section for why it is off)

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The Qt app lives in app_osyntho/, while this script sits at the repository
# root (next to the firmware's CMakeLists.txt). Everything source-related —
# version, qml/, assets/ — must be read from the app dir, so resolve it once
# here. Falls back to SCRIPT_DIR so the script keeps working if it is ever
# moved into app_osyntho/ itself.
if [ -d "$SCRIPT_DIR/app_osyntho" ]; then
    APP_SRC_DIR="$SCRIPT_DIR/app_osyntho"
else
    APP_SRC_DIR="$SCRIPT_DIR"
fi

# Name of the built executable = the CMake project()/target name in
# app_osyntho/CMakeLists.txt.
APP_NAME="osyntho"
# Store/display name: the label users actually see (Android label, macOS
# CFBundleDisplayName, .desktop Name). APP_FILE_BASE is the same identity
# without spaces, for artifact filenames.
# Defaults for the controller build; the standalone one overrides all three
# below, once $BUILD_DIR has been resolved and can be asked which it is.
APP_DISPLAY_NAME="Osyntho"
APP_FILE_BASE="Osyntho"
# Must match APP_ID in app_osyntho/CMakeLists.txt: it is the single-instance
# lock name, the macOS bundle id, the Wayland app_id the app sets on itself,
# and the .desktop/icon basename here.
APP_ID="org.osynth.osyntho"
# Read from CMakeLists.txt rather than repeated here: a hard-coded copy drifts
# from the app within a release or two, and then every artifact this script
# produces is stamped with a version the app never had.
APP_VERSION="$(sed -nE 's/^[[:space:]]*set\(APP_VERSION[[:space:]]+([0-9][0-9.]*)\).*/\1/p' "$APP_SRC_DIR/CMakeLists.txt" 2>/dev/null | head -1)"
APP_VERSION="${APP_VERSION:-0.0.0}"

# ── Colour helpers ─────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; NC='\033[0m'
die()  { echo -e "${RED}ERROR: $*${NC}" >&2; exit 1; }
info() { echo -e "${GREEN}[deploy] $*${NC}"; }
warn() { echo -e "${YELLOW}[deploy] WARNING: $*${NC}"; }
step() { echo -e "${CYAN}[deploy] >>> $*${NC}"; }

# ── Arguments ─────────────────────────────────────────────────────────────────
BUILD_DIR="${1:-}"
OUTPUT_DIR="${2:-$SCRIPT_DIR/dist-linux}"

[ -n "$BUILD_DIR" ] || die "Usage: $0 <build_dir> [output_dir]\nExample: $0 ~/build/osyntho-release ~/dist"
[ -d "$BUILD_DIR" ] || die "Build directory not found: $BUILD_DIR"
BUILD_DIR="$(realpath "$BUILD_DIR")"

# -- Which of the two builds is this? -----------------------------------------
# The controller and the standalone app are separate applications: separate
# .desktop and icon basenames, separate single-instance lock, separate data
# directory. Packaging one under the other's identity produces an AppImage that
# claims the wrong launcher entry and refuses to run while its sibling is up.
#
# CMakeCache.txt is the honest source: OSYNTHO_EMBEDDED is a cache option and the
# build tree records it nowhere else. An unreadable cache leaves the controller
# defaults in place, which is what this script always assumed.
OSYNTHO_EMBEDDED="$(awk -F= '/^OSYNTHO_EMBEDDED:BOOL=/{print $2; exit}' \
                    "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true)"
case "${OSYNTHO_EMBEDDED:-}" in
    ON|on|1|TRUE|true|YES|yes)
        APP_ID="org.osynth.osyntho.standalone"
        APP_DISPLAY_NAME="Osyntho Standalone"
        APP_FILE_BASE="Osyntho-Standalone"
        VARIANT="standalone (embedded synth engine)"
        ;;
    *)
        VARIANT="controller (BLE)"
        ;;
esac

# Resolve OUTPUT_DIR to an absolute path now, before any cd calls change the
# working directory and make relative paths resolve to the wrong location.
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(realpath "$OUTPUT_DIR")"

# Locate the executable. Qt Creator drops it at the build root, but ninja/make
# setups with a bin/ layout are common enough to be worth probing for.
EXE=""
for _cand in "$BUILD_DIR/$APP_NAME" "$BUILD_DIR/bin/$APP_NAME" "$BUILD_DIR/Release/$APP_NAME"; do
    [ -f "$_cand" ] && { EXE="$_cand"; break; }
done
if [ -z "$EXE" ]; then
    EXE="$(find "$BUILD_DIR" -maxdepth 3 -type f -name "$APP_NAME" -perm -u+x 2>/dev/null | head -1)"
fi
[ -n "$EXE" ] && [ -f "$EXE" ] \
    || die "Executable '$APP_NAME' not found under: $BUILD_DIR\nBuild app_osyntho/ first (cmake --build ...)."

# Build products (assets/, org/osynth/qmldir) are written next to the binary by
# the POST_BUILD steps in app_osyntho/CMakeLists.txt, which is not necessarily
# $BUILD_DIR itself when a bin/ layout is in use.
EXE_DIR="$(dirname "$EXE")"

ARCH="$(uname -m)"
info "App          : $APP_DISPLAY_NAME $APP_VERSION"
info "Variant      : $VARIANT"
info "App id       : $APP_ID"
info "Architecture : $ARCH"
info "App source   : $APP_SRC_DIR"
info "Build dir    : $BUILD_DIR"
info "Executable   : $EXE"
info "Output dir   : $OUTPUT_DIR"

# ── Detect Qt prefix ──────────────────────────────────────────────────────────
step "Detecting Qt installation..."

QMAKE_BIN=""
QT_PREFIX=""

# CMakeCache.txt tells us exactly which Qt this build was linked against, which
# beats whatever qmake happens to be first on PATH.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    RAW="$(awk -F'=' '/^Qt6_DIR:PATH=/{print $2; exit}' "$BUILD_DIR/CMakeCache.txt")"
    # RAW e.g. /home/user/Qt/6.11.0/gcc_64/lib/cmake/Qt6
    if [ -n "$RAW" ]; then
        CAND="${RAW%/lib/cmake/Qt6}"
        for q in "$CAND/bin/qmake6" "$CAND/bin/qmake"; do
            if [ -x "$q" ]; then
                QT_PREFIX="$CAND"; QMAKE_BIN="$q"
                info "Detected via CMakeCache.txt"
                break
            fi
        done
    fi
fi

if [ -z "$QT_PREFIX" ]; then
    for candidate in qmake6 qmake; do
        if command -v "$candidate" &>/dev/null; then
            QT_PREFIX="$("$candidate" -query QT_INSTALL_PREFIX 2>/dev/null || true)"
            QMAKE_BIN="$(command -v "$candidate")"
            break
        fi
    done
fi

# Fallback: locate Qt via ldd on the executable
if [ -z "$QT_PREFIX" ]; then
    QT_LIB="$(ldd "$EXE" 2>/dev/null | awk '/libQt6Core\.so/{print $3}' | head -1)"
    if [ -n "$QT_LIB" ] && [ -f "$QT_LIB" ]; then
        QT_PREFIX="$(realpath "$(dirname "$QT_LIB")/..")"
        for q in "$QT_PREFIX/bin/qmake6" "$QT_PREFIX/bin/qmake"; do
            [ -f "$q" ] && { QMAKE_BIN="$q"; break; }
        done
    fi
fi

if [ -n "$QT_PREFIX" ]; then
    info "Qt prefix : $QT_PREFIX"
    info "qmake     : $QMAKE_BIN"
    export QMAKE="$QMAKE_BIN"
else
    warn "Qt prefix not detected. linuxdeploy-plugin-qt may fail."
    warn "Fix by running: export QMAKE=/path/to/Qt/bin/qmake6"
fi

# ── Check required xcb libs early (fail fast before doing any heavy work) ─────
# libxcb-cursor.so.0 is a hard requirement of the Qt xcb platform plugin since
# Qt 6.5. It ships in the 'libxcb-cursor0' package, which is NOT installed by
# default on Wayland-first systems (Ubuntu 24+).
# linuxdeploy's blacklist would strip these xcb libs even if we pre-copy them.
# The fix: locate them now, copy them into AppDir AFTER linuxdeploy has finished,
# then use appimagetool to pack whatever is in AppDir (no further stripping).

_find_lib() {
    local name="$1" found
    # ldconfig cache (fast path)
    found="$(ldconfig -p 2>/dev/null | grep -F " ${name} " | grep -oP '=> \K\S+' | head -1)"
    # fallback: search common lib dirs
    [ -z "$found" ] && found="$(find \
        /usr/lib /usr/lib64 \
        /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu \
        /lib /lib64 \
        ${QT_PREFIX:+"$QT_PREFIX/lib"} \
        -maxdepth 3 -name "$name" 2>/dev/null | head -1)"
    echo "$found"
}

# Extract DT_NEEDED library names from an ELF file. Portable across mawk/gawk
# (uses [ and ] as field separators rather than gawk's 3-arg match()).
_elf_needed() {
    readelf -d "$1" 2>/dev/null | awk -F'[][]' '/NEEDED/ { print $2 }'
}

step "Checking required xcb runtime libraries..."
XCB_CURSOR_PATH="$(_find_lib libxcb-cursor.so.0)"
if [ -z "$XCB_CURSOR_PATH" ] || [ ! -f "$XCB_CURSOR_PATH" ]; then
    echo ""
    echo -e "${RED}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║  libxcb-cursor.so.0 NOT FOUND on this system.               ║${NC}"
    echo -e "${RED}║  The xcb platform plugin cannot load without it.             ║${NC}"
    echo -e "${RED}╠══════════════════════════════════════════════════════════════╣${NC}"
    echo -e "${YELLOW}║  Install it first, then re-run this script:                  ║${NC}"
    echo -e "${YELLOW}║    sudo apt-get install libxcb-cursor0                       ║${NC}"
    echo -e "${RED}╚══════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    die "Missing required library — see message above."
fi
info "Found libxcb-cursor.so.0 : $XCB_CURSOR_PATH"

# Pre-locate other xcb libs needed for portability (best-effort, not fatal)
XCB_EXTRA_PATHS=()
for LIB in libxcb-icccm.so.4 libxcb-image.so.0 libxcb-keysyms.so.1 libxcb-render-util.so.0; do
    p="$(_find_lib "$LIB")"
    if [ -n "$p" ] && [ -f "$p" ]; then
        XCB_EXTRA_PATHS+=("$p")
        info "Found $LIB : $p"
    else
        warn "$LIB not found — may be missing on minimal target systems"
    fi
done

# ── Download linuxdeploy tools ────────────────────────────────────────────────
step "Checking linuxdeploy tools..."

LINUXDEPLOY="$SCRIPT_DIR/linuxdeploy-$ARCH.AppImage"
LINUXDEPLOY_QT="$SCRIPT_DIR/linuxdeploy-plugin-qt-$ARCH.AppImage"
APPIMAGETOOL="$SCRIPT_DIR/appimagetool-$ARCH.AppImage"
APPIMAGE_RUNTIME="$SCRIPT_DIR/runtime-$ARCH"

_download() {
    local url="$1" dest="$2"
    [ -f "$dest" ] && { info "Found cached: $(basename "$dest")"; return; }
    info "Downloading $(basename "$dest") ..."
    if command -v wget &>/dev/null; then
        wget -q --show-progress -O "$dest" "$url" || die "Download failed: $url"
    elif command -v curl &>/dev/null; then
        curl -fL --progress-bar -o "$dest" "$url" || die "Download failed: $url"
    else
        die "wget/curl not found. Manually place $(basename "$dest") in:\n  $SCRIPT_DIR"
    fi
    chmod +x "$dest"
}

BASE="https://github.com/linuxdeploy"
_download "$BASE/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"          "$LINUXDEPLOY"
_download "$BASE/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage" "$LINUXDEPLOY_QT"

# appimagetool: use the newer AppImage/appimagetool repo (libfuse3-based) rather
# than the old AppImageKit repo (libfuse2-based).  Ubuntu 22+ ships libfuse3 but
# NOT libfuse2, so the old AppImageKit appimagetool fails with
# "dlopen(): error loading libfuse.so.2".
# If the cached copy is the old libfuse2 version, remove it so we re-download.
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
if [ -f "$APPIMAGETOOL" ] && \
   APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" --version 2>&1 | grep -qi 'libfuse\|fuse\.so'; then
    info "Removing cached appimagetool (old libfuse2 build)..."
    rm -f "$APPIMAGETOOL"
fi
_download "$APPIMAGETOOL_URL" "$APPIMAGETOOL"

# type2-runtime: the small binary prepended to every AppImage.
# The old runtime (bundled inside AppImageKit's appimagetool) uses libfuse.so.2,
# which is not present on Ubuntu 22+/24+ by default. The newer type2-runtime from
# AppImage/type2-runtime uses libfuse3 and also supports --appimage-extract-and-run
# without any FUSE. We pass it to appimagetool via --runtime-file so the output
# AppImage embeds this modern runtime instead of the old one.
_download "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$ARCH" \
    "$APPIMAGE_RUNTIME"

# If FUSE is unavailable, set the extract-and-run env var for all AppImage tools.
# linuxdeploy's runtime (type2, newer) supports APPIMAGE_EXTRACT_AND_RUN.
"$LINUXDEPLOY" --appimage-help >/dev/null 2>&1 || export APPIMAGE_EXTRACT_AND_RUN=1

# ── Build AppDir ──────────────────────────────────────────────────────────────
step "Building AppDir..."

APPDIR="$OUTPUT_DIR/AppDir"
# Only the AppDir, NEVER $OUTPUT_DIR: that is argument 2, so this used to
# "rm -rf" whatever the caller pointed at (~/dist, ., ~ ...) rather than just
# the staging directory this script owns.
rm -rf "$APPDIR"
mkdir -p \
    "$APPDIR/usr/bin" \
    "$APPDIR/usr/share/applications" \
    "$APPDIR/usr/share/icons/hicolor/512x512/apps" \
    "$APPDIR/usr/share/icons/hicolor/256x256/apps"

# Executable
cp "$EXE" "$APPDIR/usr/bin/$APP_NAME"
info "Copied executable"

# assets/ from the build dir. Osyntho loads every asset from the compiled-in
# resources (:/assets/... via main_resources.qrc), so this is belt-and-braces
# for the graph.svg/graph.ico copies the CMake POST_BUILD step drops next to the
# binary — a few KB, and it keeps the AppDir layout identical to the build tree.
if [ -d "$EXE_DIR/assets" ]; then
    cp -r "$EXE_DIR/assets" "$APPDIR/usr/bin/"
    info "Copied assets/"
fi

# QML module dir. app_osyntho/CMakeLists.txt copies an empty assets/qmldir to
# <build>/org/osynth/qmldir precisely because the QML engine looks for that path
# next to the executable when resolving the org.osynth.osyntho module; without
# it the engine reports the module as not installed.
if [ -f "$EXE_DIR/org/osynth/qmldir" ]; then
    mkdir -p "$APPDIR/usr/bin/org/osynth"
    cp "$EXE_DIR/org/osynth/qmldir" "$APPDIR/usr/bin/org/osynth/qmldir"
    info "Copied org/osynth/qmldir"
else
    warn "org/osynth/qmldir not found next to the executable — QML module resolution may fail"
fi

# ── App icon ──────────────────────────────────────────────────────────────────
ICON_512="$APPDIR/usr/share/icons/hicolor/512x512/apps/$APP_ID.png"
ICON_256="$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.png"

# Best source: the 1024-pixel Android icon already in the repo
if [ -f "$APP_SRC_DIR/assets/android-build/1024.png" ]; then
    cp "$APP_SRC_DIR/assets/android-build/1024.png" "$ICON_512"
    info "Using assets/android-build/1024.png as icon"
fi

# Resize to 256 if imagemagick is around; otherwise just copy
if [ -f "$ICON_512" ]; then
    if command -v convert &>/dev/null; then
        convert "$ICON_512" -resize 256x256 "$ICON_256" 2>/dev/null && info "Resized icon → 256x256" || cp "$ICON_512" "$ICON_256"
    else
        cp "$ICON_512" "$ICON_256"
    fi
fi

# Fallback: convert SVG → PNG (graph.svg is also the in-app window icon)
if [ ! -f "$ICON_256" ] && [ -f "$APP_SRC_DIR/assets/graph.svg" ]; then
    if command -v rsvg-convert &>/dev/null; then
        rsvg-convert -w 256 -h 256 "$APP_SRC_DIR/assets/graph.svg" -o "$ICON_256" 2>/dev/null \
            && info "Converted graph.svg → icon" || warn "rsvg-convert failed, no icon embedded"
    elif command -v inkscape &>/dev/null; then
        inkscape --export-type=png --export-filename="$ICON_256" -w 256 -h 256 \
            "$APP_SRC_DIR/assets/graph.svg" 2>/dev/null && info "inkscape: SVG → icon" || true
    else
        warn "No PNG icon found and no SVG converter available."
        warn "Install librsvg2-bin (rsvg-convert) or imagemagick for an icon."
    fi
fi

# ── Desktop entry ─────────────────────────────────────────────────────────────
DESKTOP_FILE="$APPDIR/usr/share/applications/$APP_ID.desktop"
# StartupWMClass is the X11 half of associating the running window with this
# entry; without it the taskbar shows a generic icon next to the launcher's
# real one. Qt sets WM_CLASS to <argv[0] basename> NUL <applicationName>, and the
# executable is called "osyntho" in both builds -- so the class (the second
# field, i.e. applicationName = APP_DISPLAY_NAME) is the only part that tells
# the two apart, and it is what goes here. Wayland does not use it: the app
# calls setDesktopFileName(APP_ID) and the compositor matches this file by name.
cat > "$DESKTOP_FILE" << EOF
[Desktop Entry]
Type=Application
Name=$APP_DISPLAY_NAME
Exec=$APP_NAME
Icon=$APP_ID
Categories=AudioVideo;Audio;Music;
Comment=Companion app for the Osynth synthesizer
Keywords=synth;synthesizer;midi;bluetooth;osynth;
StartupWMClass=$APP_DISPLAY_NAME
EOF
info "Created .desktop file"

# ── Step 1: Qt plugin — populates AppDir with Qt libs/plugins/QML ─────────────
step "Step 1/3: linuxdeploy-plugin-qt (populating AppDir)..."

# QML_SOURCES_PATHS tells linuxdeploy-plugin-qt where to scan for `import`
# statements, so it knows which QML modules (QtCore, QtQuick.Controls.Material,
# QtQuick.Dialogs, etc.) to copy into AppDir/usr/qml. Without this the QML
# engine fails to load Main.qml with "module ... is not installed".
QML_SOURCES_PATHS_LIST=""
for _qml_dir in "$APP_SRC_DIR/qml" "$APP_SRC_DIR"; do
    if [ -d "$_qml_dir" ]; then
        QML_SOURCES_PATHS_LIST="${QML_SOURCES_PATHS_LIST:+${QML_SOURCES_PATHS_LIST}:}$_qml_dir"
    fi
done
info "QML sources path: $QML_SOURCES_PATHS_LIST"

# EXTRA_QT_MODULES mirrors target_link_libraries() in app_osyntho/CMakeLists.txt.
# 'multimedia' is deliberately absent — see the multimedia section below.
EXTRA_QT_MODULES="bluetooth;sql;network;concurrent;dbus" \
EXTRA_QT_PLUGINS="platforms" \
QML_SOURCES_PATHS="$QML_SOURCES_PATHS_LIST" \
DEPLOY_PLATFORM_THEMES=1 \
"$LINUXDEPLOY_QT" --appdir "$APPDIR" \
    || warn "Qt plugin step had warnings (continuing)"

# In the split two-step flow (Qt plugin standalone → linuxdeploy), the Qt
# plugin may not create the apprun-hooks snippet that sets Qt platform paths.
# Write it explicitly so linuxdeploy's generated AppRun always sources it.
# Without this, QT_QPA_PLATFORM_PLUGIN_PATH stays empty and the app aborts
# with "no Qt platform plugin could be initialized".
mkdir -p "$APPDIR/apprun-hooks"
cat > "$APPDIR/apprun-hooks/linuxdeploy-plugin-qt-hook.sh" << 'HOOK'
export QT_QPA_PLATFORM_PLUGIN_PATH="${APPDIR}/usr/plugins/platforms"
export QT_PLUGIN_PATH="${APPDIR}/usr/plugins"
export QML2_IMPORT_PATH="${APPDIR}/usr/qml${QML2_IMPORT_PATH:+:${QML2_IMPORT_PATH}}"
export QML_IMPORT_PATH="${APPDIR}/usr/qml${QML_IMPORT_PATH:+:${QML_IMPORT_PATH}}"
HOOK
info "Wrote apprun-hooks/linuxdeploy-plugin-qt-hook.sh"

# Defensive backstop: explicitly copy the QML modules the app imports.
# Even with QML_SOURCES_PATHS set, linuxdeploy-plugin-qt sometimes misses
# style modules (Material) and modules imported transitively. Copy these
# unconditionally — they are small and the cost of missing them is the
# QML engine refusing to load Main.qml.
# The list is the union of `import` lines under app_osyntho/qml/: QtQuick,
# QtQuick.Controls(.Material), QtQuick.Layouts, QtQuick.Window, QtQuick.Dialogs
# and QtCore (for Settings, used by WindowStateSaver.qml). Copying QtQuick also
# pulls its nested Controls/Layouts/Window/Dialogs/Templates subdirs; the
# per-module entries below are there in case a partial QtQuick tree already
# exists. Add a line here if a new import shows up in the QML.
if [ -n "$QT_PREFIX" ] && [ -d "$QT_PREFIX/qml" ]; then
    mkdir -p "$APPDIR/usr/qml"
    for QML_MOD in \
        QtCore \
        QtQml \
        QtQuick \
        QtQuick/Controls \
        QtQuick/Controls/Material \
        QtQuick/Controls/impl \
        QtQuick/Controls/Material/impl \
        QtQuick/Dialogs \
        QtQuick/Dialogs/quickimpl \
        QtQuick/Templates \
        QtQuick/Layouts \
        QtQuick/Window; do
        SRC="$QT_PREFIX/qml/$QML_MOD"
        DEST="$APPDIR/usr/qml/$QML_MOD"
        if [ -d "$SRC" ] && [ ! -d "$DEST" ]; then
            mkdir -p "$(dirname "$DEST")"
            cp -r "$SRC" "$DEST"
            info "Copied QML module: $QML_MOD"
        fi
    done
fi

# Explicitly copy platform plugins from the Qt installation.
# linuxdeploy-plugin-qt should deploy these automatically, but in the two-step
# flow it may exit before doing so (e.g. if another plugin triggers an error).
# Copying them here guarantees they're present; linuxdeploy (step 2) then
# resolves their shared-library dependencies (libxcb-*, libGL, etc.).
if [ -n "$QT_PREFIX" ] && [ -d "$QT_PREFIX/plugins/platforms" ]; then
    mkdir -p "$APPDIR/usr/plugins/platforms"
    PLATFORM_COPIED=0
    for PLG in \
        libqxcb.so \
        libqwayland-generic.so \
        libqwayland-egl.so \
        libqwayland-xcomposite-egl.so \
        libqwayland-xcomposite-glx.so \
        libqminimal.so \
        libqoffscreen.so \
        libqeglfs.so \
        libqvnc.so; do
        SRC="$QT_PREFIX/plugins/platforms/$PLG"
        if [ -f "$SRC" ]; then
            cp "$SRC" "$APPDIR/usr/plugins/platforms/"
            PLATFORM_COPIED=$((PLATFORM_COPIED + 1))
        fi
    done
    info "Copied $PLATFORM_COPIED platform plugin(s) from $QT_PREFIX/plugins/platforms"
    # xcb also needs GL integration plugins
    if [ -d "$QT_PREFIX/plugins/xcbglintegrations" ]; then
        mkdir -p "$APPDIR/usr/plugins/xcbglintegrations"
        cp "$QT_PREFIX/plugins/xcbglintegrations"/*.so "$APPDIR/usr/plugins/xcbglintegrations/" 2>/dev/null || true
        info "Copied xcbglintegrations plugins"
    fi
else
    warn "Qt plugins/platforms dir not found — platform plugins may be missing from AppImage"
fi

# Copy supporting plugin categories that Qt loads at runtime. Missing any of
# these manifests as features silently failing (e.g. iconengines missing → the
# SVG window icon doesn't render; tls missing → https stops working). Cheap to
# ship.
if [ -n "$QT_PREFIX" ]; then
    for PLUGIN_CAT in imageformats iconengines tls networkinformation generic; do
        SRC_DIR="$QT_PREFIX/plugins/$PLUGIN_CAT"
        DEST_DIR="$APPDIR/usr/plugins/$PLUGIN_CAT"
        if [ -d "$SRC_DIR" ] && [ ! -d "$DEST_DIR" ]; then
            cp -r "$SRC_DIR" "$DEST_DIR"
            info "Copied plugin category: $PLUGIN_CAT"
        fi
    done
fi

# Show what landed in platforms/ so failures are easy to diagnose
PLATFORMS_IN_APPDIR="$(find "$APPDIR/usr/plugins/platforms" -name "*.so" 2>/dev/null | sort | xargs -r basename -a | tr '\n' ' ')"
info "Platform plugins in AppDir: ${PLATFORMS_IN_APPDIR:-NONE}"

# Remove optional SQL drivers whose system libs are unlikely to be installed.
# The app only uses SQLite (libqsqlite.so — Database::addDatabase("QSQLITE")),
# which has no external dependencies. Leaving ibase/odbc/psql/mysql in AppDir
# causes linuxdeploy to fail when it tries to resolve libfbclient.so.2,
# unixodbc, libpq, libmysqlclient, etc.
step "Removing optional SQL drivers (ibase/odbc/psql/mysql)..."
for DRV in libqsqlibase.so libqsqlodbc.so libqsqlpsql.so libqsqlmysql.so; do
    FOUND_DRV="$(find "$APPDIR" -name "$DRV" 2>/dev/null)"
    if [ -n "$FOUND_DRV" ]; then
        rm -f $FOUND_DRV
        info "Removed $DRV"
    fi
done

# ── QtMultimedia backend (opt-in) ─────────────────────────────────────────────
# app_osyntho links Qt6::Multimedia, but nothing in the shipped build actually
# constructs a media object: src/camerapreviewitem.cpp is the only user and it
# is NOT in the qt_add_qml_module SOURCES list, and no QML file imports
# QtMultimedia. libQt6Multimedia.so.6 still comes along as a DT_NEEDED of the
# executable, which is fine — the backend plugin is what drags in ~100 MB of
# Qt-shipped FFmpeg libs, and without a media object nothing ever asks for it.
#
# So: skip it by default, and keep the full (recursive) bundling path behind
# BUNDLE_MULTIMEDIA=1 for the day audio playback or the camera preview is wired
# up. The symptom of needing it is "No QtMultimedia backends found." at runtime.
if [ "${BUNDLE_MULTIMEDIA:-0}" = "1" ] && [ -n "$QT_PREFIX" ] && [ -d "$QT_PREFIX/plugins/multimedia" ]; then
    step "Bundling QtMultimedia backend (BUNDLE_MULTIMEDIA=1)..."
    mkdir -p "$APPDIR/usr/plugins/multimedia"
    _mm_copied=0
    for MM in libffmpegmediaplugin.so libgstreamermediaplugin.so; do
        SRC="$QT_PREFIX/plugins/multimedia/$MM"
        DEST="$APPDIR/usr/plugins/multimedia/$MM"
        if [ -f "$SRC" ] && [ ! -f "$DEST" ]; then
            cp "$SRC" "$DEST"
            info "Copied multimedia backend: $MM"
            _mm_copied=$((_mm_copied + 1))
        fi
    done
    [ "$_mm_copied" -eq 0 ] && warn "No multimedia backend plugins copied — media features will fail"

    # Bundle the FFmpeg libs the plugin needs at runtime. Qt 6.5+ ships its
    # own FFmpeg in $QT_PREFIX/lib at exact SONAMEs the plugin was built
    # against. linuxdeploy doesn't follow this branch because the plugin is
    # loaded dynamically and is not in the executable's DT_NEEDED tree.
    # Use `cp -L` to dereference any SONAME-version symlinks so the final
    # file in usr/lib has the exact name the plugin's DT_NEEDED expects.
    if [ -f "$APPDIR/usr/plugins/multimedia/libffmpegmediaplugin.so" ]; then
        info "Bundling FFmpeg libs needed by the multimedia plugin..."
        # usr/lib may not exist yet at this point in the script (the xcb
        # bundling step is what normally creates it). Create it now.
        mkdir -p "$APPDIR/usr/lib"
        # Visited set so we recurse into libav*'s own deps (e.g. libavcodec
        # depends on libavutil) without infinite loops.
        declare -A _FFMPEG_VISITED

        _bundle_ffmpeg_dep() {
            local libname="$1"
            [ -n "${_FFMPEG_VISITED[$libname]}" ] && return 0
            _FFMPEG_VISITED[$libname]=1
            local src=""
            local d
            for d in \
                "$QT_PREFIX/lib" \
                "$QT_PREFIX/lib/x86_64-linux-gnu" \
                "$QT_PREFIX/lib/aarch64-linux-gnu" \
                /usr/lib/x86_64-linux-gnu \
                /usr/lib; do
                if [ -f "$d/$libname" ]; then
                    src="$d/$libname"
                    break
                fi
            done
            if [ -z "$src" ]; then
                warn "  FFmpeg dep $libname not found — multimedia plugin will fail at runtime"
                return 0
            fi
            if [ ! -f "$APPDIR/usr/lib/$libname" ]; then
                if cp -L "$src" "$APPDIR/usr/lib/$libname"; then
                    info "  bundled FFmpeg dep: $libname (from $src)"
                else
                    warn "  FAILED to copy $src -> $APPDIR/usr/lib/$libname"
                    return 0
                fi
            fi
            local _dn
            while IFS= read -r _dn; do
                case "$_dn" in
                    libav*.so.*|libsw*.so.*|libpostproc*.so.*) _bundle_ffmpeg_dep "$_dn" ;;
                esac
            done < <(_elf_needed "$src")
            return 0
        }

        set +e
        # 1. Seed from DT_NEEDED entries (libavformat/codec/util/swscale/swresample)
        while IFS= read -r dep_name; do
            case "$dep_name" in
                libav*.so.*|libsw*.so.*|libpostproc*.so.*) _bundle_ffmpeg_dep "$dep_name" ;;
            esac
        done < <(_elf_needed "$APPDIR/usr/plugins/multimedia/libffmpegmediaplugin.so")

        # 2. Additionally bundle FFmpeg libs that are dlopen()'d at runtime,
        # not DT_NEEDED — most importantly libavdevice (capture) and
        # libavfilter (frame processing). Without these the FFmpeg backend
        # opens the device but never delivers frames to Qt.
        # Glob the Qt-shipped libs directly: anything starting with libav*,
        # libsw*, or libpostproc* in QT_PREFIX/lib is fair game.
        for ffmpeg_lib in "$QT_PREFIX/lib"/libav*.so.* \
                          "$QT_PREFIX/lib"/libsw*.so.* \
                          "$QT_PREFIX/lib"/libpostproc*.so.*; do
            [ -f "$ffmpeg_lib" ] || continue
            # Skip the unversioned symlink ('libavformat.so' with no number)
            # so we always bundle the versioned SONAME the plugin asks for.
            case "$(basename "$ffmpeg_lib")" in
                *.so) continue ;;
            esac
            # Only bundle the SONAME-versioned file (e.g. libavformat.so.61),
            # not the fully-versioned one (libavformat.so.61.7.100). The
            # plugin's DT_NEEDED references the SONAME-versioned form.
            base="$(basename "$ffmpeg_lib")"
            # Count dots after .so. — 1 dot means SONAME version (.so.X)
            suffix="${base#*.so.}"
            case "$suffix" in
                *.*) continue ;;  # full version like 61.7.100 — skip
            esac
            _bundle_ffmpeg_dep "$base"
        done
        set -e
    fi
else
    # linuxdeploy-plugin-qt may have deployed the backend anyway (it keys off
    # libQt6Multimedia being present). Drop it and any FFmpeg libs it pulled in,
    # BEFORE the dependency scan below — otherwise the scan walks the plugin's
    # DT_NEEDED tree and drags the whole of FFmpeg back in.
    if [ -d "$APPDIR/usr/plugins/multimedia" ]; then
        rm -rf "$APPDIR/usr/plugins/multimedia"
        info "Dropped QtMultimedia backend plugins (BUNDLE_MULTIMEDIA=1 to keep them)"
    fi
    _mm_dropped=0
    for _ff in "$APPDIR/usr/lib"/libav*.so.* "$APPDIR/usr/lib"/libsw*.so.* \
               "$APPDIR/usr/lib"/libpostproc*.so.*; do
        [ -f "$_ff" ] || continue
        rm -f "$_ff"
        _mm_dropped=$((_mm_dropped + 1))
    done
    [ "$_mm_dropped" -gt 0 ] && info "Dropped $_mm_dropped Qt-shipped FFmpeg lib(s) — unused by this app"
fi

# Explicitly bundle platform theme plugins so the app uses native file dialogs.
# Two dialog paths exist on Linux and both benefit: src/nativefiledialog.cpp
# shells out to zenity (NFD_AVAILABLE), and QtQuick.Dialogs.FileDialog is used
# where the wrapper isn't. Without one of these plugins the QML FileDialog
# silently falls back to its non-native Qt Quick implementation. The XDG Desktop
# Portal plugin is the best fit for a portable AppImage: tiny, requires no
# GTK/KDE libs to be bundled, and uses DBus to talk to the system's
# xdg-desktop-portal service (default on Ubuntu 22+, Fedora, most modern
# desktops). libqgtk3.so is the fallback for GNOME systems without the portal.
if [ -n "$QT_PREFIX" ] && [ -d "$QT_PREFIX/plugins/platformthemes" ]; then
    mkdir -p "$APPDIR/usr/plugins/platformthemes"
    _theme_copied=0
    for THEME in libqxdgdesktopportal.so libqgtk3.so; do
        SRC="$QT_PREFIX/plugins/platformthemes/$THEME"
        DEST="$APPDIR/usr/plugins/platformthemes/$THEME"
        if [ -f "$SRC" ] && [ ! -f "$DEST" ]; then
            cp "$SRC" "$DEST"
            info "Copied platform theme: $THEME"
            _theme_copied=$((_theme_copied + 1))
        fi
    done
    if [ "$_theme_copied" -eq 0 ] && [ -z "$(ls "$APPDIR/usr/plugins/platformthemes/" 2>/dev/null)" ]; then
        warn "No platform theme plugins available — native file dialogs will fall back to Qt Quick"
    fi
fi

# Explicitly ensure the SQLite driver is bundled. linuxdeploy-plugin-qt usually
# deploys it automatically because the app links libQt6Sql.so.6, but in the
# two-step flow it sometimes misses sqldrivers/ entirely — and without
# libqsqlite.so Database::open() fails the isDriverAvailable("QSQLITE") check
# and the app starts with no settings, presets or patch library.
if [ -n "$QT_PREFIX" ] && [ -f "$QT_PREFIX/plugins/sqldrivers/libqsqlite.so" ]; then
    mkdir -p "$APPDIR/usr/plugins/sqldrivers"
    if [ ! -f "$APPDIR/usr/plugins/sqldrivers/libqsqlite.so" ]; then
        cp "$QT_PREFIX/plugins/sqldrivers/libqsqlite.so" "$APPDIR/usr/plugins/sqldrivers/"
        info "Copied libqsqlite.so (SQLite driver)"
    else
        info "libqsqlite.so already present in AppDir"
    fi
else
    warn "libqsqlite.so not found in $QT_PREFIX/plugins/sqldrivers/ — SQLite will fail at runtime"
fi

# ── Step 2: linuxdeploy — resolve all remaining library dependencies ───────────
# No --output appimage here: we let linuxdeploy fully prepare AppDir (deploy
# Qt and system lib dependencies, create AppRun), but we do NOT ask it to pack
# the AppImage yet. This way we can still add xcb libs in the next step,
# after linuxdeploy has finished and its blacklist can no longer strip them.
step "Step 2/3: linuxdeploy (resolving library dependencies)..."

"$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --desktop-file "$DESKTOP_FILE" \
    --executable "$APPDIR/usr/bin/$APP_NAME" \
    || warn "linuxdeploy exited non-zero (check output above)"

# Ensure the AppDir-root desktop symlink exists. linuxdeploy normally creates
# it, but when linuxdeploy exits non-zero (e.g., a broken ELF among the libs
# we pre-copied) it skips this step — and appimagetool then aborts with
# "Desktop file not found". Recreate the symlink defensively.
if [ -f "$DESKTOP_FILE" ] && [ ! -e "$APPDIR/$(basename "$DESKTOP_FILE")" ]; then
    ln -sf "usr/share/applications/$(basename "$DESKTOP_FILE")" \
           "$APPDIR/$(basename "$DESKTOP_FILE")"
    info "Recreated AppDir-root desktop symlink (linuxdeploy didn't)"
fi
# Same defensive recreation for the icon symlink appimagetool needs.
ICON_AT_ROOT="$APPDIR/$APP_ID.png"
if [ ! -e "$ICON_AT_ROOT" ]; then
    for ICON_SRC in \
        "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.png" \
        "$APPDIR/usr/share/icons/hicolor/512x512/apps/$APP_ID.png"; do
        if [ -f "$ICON_SRC" ]; then
            ln -sf "${ICON_SRC#$APPDIR/}" "$ICON_AT_ROOT"
            info "Recreated AppDir-root icon symlink ($(basename "$ICON_SRC"))"
            break
        fi
    done
fi

# ── Scan plugins for missing Qt library deps ──────────────────────────────────
# Gap in the two-step flow: linuxdeploy-plugin-qt (Step 1) ran BEFORE we copied
# platform plugins, so Qt-internal libs needed by those plugins — most notably
# libQt6XcbQpa.so.6 required by libqxcb.so — were never deployed. linuxdeploy
# (Step 2) skips all libQt*.so.* on the assumption the Qt plugin already handled
# them. Close the gap: scan every .so under usr/plugins/ and copy any missing
# Qt libs directly from QT_PREFIX/lib.
step "Scanning plugins for missing Qt library deps..."
# Disable `set -e` for the scan. The recursion exercises many `[[ ]]` tests
# inside while-loop BODIES, and bash's set -e is NOT suppressed inside loop
# bodies (only inside their condition). A false `[[ ]]` would silently abort
# the script mid-recursion. Restored after the scan completes.
set +e
if [ -n "$QT_PREFIX" ]; then
    declare -A _QT_DEP_VISITED

    # Extract DT_NEEDED library names from an ELF file.
    # Uses '[' and ']' as awk field separators so the lib name (between the
    # brackets) is field 2. Portable across mawk/gawk/busybox awk — unlike
    # gawk's 3-arg match() which mawk silently no-ops.
    _elf_needed() {
        readelf -d "$1" 2>/dev/null | awk -F'[][]' '/NEEDED/ { print $2 }'
    }

    _bundle_qt_dep() {
        local libname="$1"
        if [ -n "${_QT_DEP_VISITED[$libname]}" ]; then
            return 0
        fi
        _QT_DEP_VISITED[$libname]=1
        # Locate source: prefer QT_PREFIX (so we copy Qt-shipped version), fall
        # back to the AppDir copy so we can still RECURSE into its deps even
        # when the file itself isn't a Qt-shipped lib we'd bundle.
        local src=""
        local _qt_lib_dir
        for _qt_lib_dir in \
            "$QT_PREFIX/lib" \
            "$QT_PREFIX/lib/x86_64-linux-gnu" \
            "$QT_PREFIX/lib/aarch64-linux-gnu"; do
            if [ -f "$_qt_lib_dir/$libname" ]; then
                src="$_qt_lib_dir/$libname"
                break
            fi
        done
        local found_in_qt=0
        if [ -n "$src" ]; then
            found_in_qt=1
        elif [ -f "$APPDIR/usr/lib/$libname" ]; then
            # Already in AppDir (linuxdeploy or earlier step put it there) —
            # recurse via that copy so we still discover its transitive deps.
            src="$APPDIR/usr/lib/$libname"
        else
            return 0  # Not a Qt-shipped lib, leave to the target system.
        fi
        if [ "$found_in_qt" -eq 1 ] && [ ! -f "$APPDIR/usr/lib/$libname" ]; then
            cp -L "$src" "$APPDIR/usr/lib/$libname"
            info "  bundled missing Qt-shipped dep: $libname (from $src)"
        fi
        # Recurse through ALL deps, not just libQt*. This catches libicu*,
        # libav*, libsw*, etc. that Qt ships alongside its own libs.
        local _dep_name
        while IFS= read -r _dep_name; do
            case "$_dep_name" in
                *.so.*) _bundle_qt_dep "$_dep_name" ;;
            esac
        done < <(_elf_needed "$src")
        return 0
    }

    # readelf -d gives DT_NEEDED names directly from the ELF binary, so we
    # don't depend on ldconfig knowing where Qt lives (which ldd does need).
    if ! command -v readelf >/dev/null 2>&1; then
        warn "readelf not found (install 'binutils') — cannot scan for missing Qt deps"
    else
        _so_count=0
        # Also seed the scan from libraries already in usr/lib/ — linuxdeploy
        # may have placed libQt6Core.so.6 etc. there but skipped their non-Qt
        # transitive deps (libicu*, …). The function dedupes via visited set.
        while IFS= read -r so_file; do
            [ -f "$so_file" ] || continue
            _so_count=$((_so_count + 1))
            while IFS= read -r dep_name; do
                case "$dep_name" in
                    *.so.*) _bundle_qt_dep "$dep_name" ;;
                esac
            done < <(_elf_needed "$so_file")
        done < <(find "$APPDIR/usr/plugins" "$APPDIR/usr/qml" "$APPDIR/usr/lib" \
                      -name "*.so*" -type f 2>/dev/null)
        info "Scanned $_so_count plugin/qml-module/lib .so(s); visited ${#_QT_DEP_VISITED[@]} dep name(s)"
    fi
else
    warn "QT_PREFIX not set — cannot scan plugins for missing Qt deps (libQt6XcbQpa.so.6 may be absent)"
fi
set -e

# ── Force-bundle xcb libs (AFTER linuxdeploy, so they cannot be stripped) ─────
# linuxdeploy's internal blacklist excludes most libxcb-* libs on the assumption
# they are always present on the target. That assumption fails for
# libxcb-cursor.so.0, which is required since Qt 6.5 but not installed by
# default on Wayland-first distros. Copying them here (after linuxdeploy has
# already run) means appimagetool will pack them verbatim — nothing strips them.
step "Force-bundling xcb libs into AppDir (post-linuxdeploy)..."
mkdir -p "$APPDIR/usr/lib"

# Bundle xcb/X11 transitive dependencies using a visited-name set.
# Bug in the previous approach: early-return on "already in AppDir" was WRONG
# because linuxdeploy-plugin-qt copies some xcb libs (e.g. libxcb-image.so.0)
# WITHOUT their own transitive deps (e.g. libxcb-util.so.1 — also on the
# blacklist). Keying the visited set on the NAME (not on presence in AppDir)
# ensures we always recurse into deps of every lib we encounter for the first
# time, even if linuxdeploy already placed it there without its own deps.
declare -A _XCB_VISITED

_bundle_xcb_dep() {
    local libname="$1"
    [[ -n "${_XCB_VISITED[$libname]}" ]] && return
    _XCB_VISITED[$libname]=1

    # Locate the HOST copy so ldd runs against the unmodified system library
    local host_path
    host_path="$(_find_lib "$libname")"
    if [ -z "$host_path" ] || [ ! -f "$host_path" ]; then
        warn "  _bundle_xcb_dep: cannot find $libname on host — skipping"
        return
    fi

    if [ ! -f "$APPDIR/usr/lib/$libname" ]; then
        cp "$host_path" "$APPDIR/usr/lib/"
        info "  bundled: $libname"
    fi

    while IFS= read -r dep; do
        [ -n "$dep" ] && [ -f "$dep" ] || continue
        case "$(basename "$dep")" in
            libxcb*.so.*|libXau.so.*|libXdmcp.so.*)
                _bundle_xcb_dep "$(basename "$dep")" ;;
        esac
    done < <(ldd "$host_path" 2>/dev/null | awk '/=> \// { print $3 }')
}

# Seed from xcb-cursor (required since Qt 6.5) and any extra xcb libs
_bundle_xcb_dep "$(basename "$XCB_CURSOR_PATH")"
for xcb_path in "${XCB_EXTRA_PATHS[@]}"; do
    _bundle_xcb_dep "$(basename "$xcb_path")"
done

# Also seed from everything libqxcb.so depends on — catches any xcb lib the
# platform plugin needs that wasn't reachable from the cursor dep chain
if [ -f "$APPDIR/usr/plugins/platforms/libqxcb.so" ]; then
    while IFS= read -r dep; do
        [ -n "$dep" ] && [ -f "$dep" ] || continue
        case "$(basename "$dep")" in
            libxcb*.so.*|libXau.so.*|libXdmcp.so.*)
                _bundle_xcb_dep "$(basename "$dep")" ;;
        esac
    done < <(ldd "$APPDIR/usr/plugins/platforms/libqxcb.so" 2>/dev/null | awk '/=> \// { print $3 }')
fi

# Bundle non-xcb transitive deps of libxcb-cursor.so.0 (libbsd, libmd).
# These are needed by xcb-cursor but not matched by the libxcb*.so.* filter.
# They are present on most Ubuntu installs but missing on minimal/container systems.
info "Bundling non-xcb deps of libxcb-cursor.so.0..."
for _nondep in libbsd.so.0 libmd.so.0; do
    _np="$(_find_lib "$_nondep")"
    if [ -n "$_np" ] && [ -f "$_np" ]; then
        if [ ! -f "$APPDIR/usr/lib/$_nondep" ]; then
            cp "$_np" "$APPDIR/usr/lib/"
            info "  bundled: $_nondep"
        else
            info "  already present: $_nondep"
        fi
    else
        warn "  $_nondep not found on host — skipping"
    fi
done

# ── Custom AppRun (replaces linuxdeploy's generated one) ──────────────────────
# linuxdeploy's generated AppRun may not set LD_LIBRARY_PATH in a way that
# covers AppDir/usr/lib when the xcb platform plugin does its own dlopen() for
# libxcb-cursor.so.0 at init time. Writing an explicit AppRun here (after
# linuxdeploy has finished all processing) guarantees LD_LIBRARY_PATH is correct.
# appimagetool will pack this AppRun as-is.
step "Writing custom AppRun..."
cat > "$APPDIR/AppRun" << APPRUN_EOF
#!/bin/bash
APPDIR="\$(dirname "\$(readlink -f "\${0}")")"
export APPDIR

export PATH="\${APPDIR}/usr/bin:\${APPDIR}/usr/sbin\${PATH:+:\${PATH}}"
export LD_LIBRARY_PATH="\${APPDIR}/usr/lib:\${APPDIR}/usr/lib/x86_64-linux-gnu\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
export XDG_DATA_DIRS="\${APPDIR}/usr/share\${XDG_DATA_DIRS:+:\${XDG_DATA_DIRS}}"

# Ubuntu 24.04 defaults to Wayland. If DISPLAY is unset but WAYLAND_DISPLAY is
# set, XWayland is almost certainly running at :0 — set it so the xcb platform
# plugin can connect to X11 as a fallback when no Wayland plugin is bundled.
if [ -z "\${DISPLAY}" ] && [ -n "\${WAYLAND_DISPLAY}" ]; then
    export DISPLAY=:0
fi

# Explicitly select the xcb platform plugin (avoids ambiguity when both xcb
# and wayland entries are present in the plugin dir). Leave QSG_RENDER_LOOP
# unset so Qt picks the default ("threaded"): main.cpp reads GL_RENDERER from
# the scene graph's OpenGL context to decide the graph paint path, and the
# knobs/keyboard/graph screens are all animated. Set QSG_RENDER_LOOP=basic
# before launching if you hit OpenGL trouble in a VM without 3D acceleration.
export QT_QPA_PLATFORM="\${QT_QPA_PLATFORM:-xcb}"

# Prefer the XDG Desktop Portal platform theme so QtQuick.Dialogs.FileDialog
# uses the system-native dialog (via xdg-desktop-portal). Falls back to gtk3
# if the portal plugin isn't present. Set QT_QPA_PLATFORMTHEME=<empty> to skip.
# Note: the .olt/.wav import & export paths go through src/nativefiledialog.cpp,
# which shells out to zenity instead — see the runtime notes in the deploy log.
if [ -z "\${QT_QPA_PLATFORMTHEME+set}" ]; then
    if [ -f "\${APPDIR}/usr/plugins/platformthemes/libqxdgdesktopportal.so" ]; then
        export QT_QPA_PLATFORMTHEME=xdgdesktopportal
    elif [ -f "\${APPDIR}/usr/plugins/platformthemes/libqgtk3.so" ]; then
        export QT_QPA_PLATFORMTHEME=gtk3
    fi
fi

# Source Qt environment hooks (QT_PLUGIN_PATH, QT_QPA_PLATFORM_PLUGIN_PATH, etc.)
for hook in "\${APPDIR}/apprun-hooks"/*.sh; do
    [ -f "\$hook" ] && source "\$hook"
done

# Debug mode: OSYNTHO_DEBUG=1 ./Osyntho-*.AppImage
# Prints env vars and enables Qt plugin tracing to help diagnose startup
# failures. Add OSYNTHO_DEBUG_BLE=1 for Qt Bluetooth logging on top of that.
if [ "\${OSYNTHO_DEBUG:-0}" = "1" ]; then
    echo "[osyntho-debug] ── environment ──────────────────────────────────"
    echo "[osyntho-debug] APPDIR            = \$APPDIR"
    echo "[osyntho-debug] DISPLAY           = \${DISPLAY:-<unset>}"
    echo "[osyntho-debug] WAYLAND_DISPLAY   = \${WAYLAND_DISPLAY:-<unset>}"
    echo "[osyntho-debug] XAUTHORITY        = \${XAUTHORITY:-<unset>}"
    echo "[osyntho-debug] QT_QPA_PLATFORM   = \$QT_QPA_PLATFORM"
    echo "[osyntho-debug] QSG_RENDER_LOOP   = \$QSG_RENDER_LOOP"
    echo "[osyntho-debug] LD_LIBRARY_PATH   = \$LD_LIBRARY_PATH"
    echo "[osyntho-debug] QT_PLUGIN_PATH    = \$QT_PLUGIN_PATH"
    echo "[osyntho-debug] QT_QPA_PLATFORM_PLUGIN_PATH = \$QT_QPA_PLATFORM_PLUGIN_PATH"
    echo "[osyntho-debug] zenity            = \$(command -v zenity || echo '<not installed — file dialogs degrade>')"
    echo "[osyntho-debug] ── platform plugins ─────────────────────────────"
    ls -1 "\${APPDIR}/usr/plugins/platforms/" 2>/dev/null | sed 's/^/[osyntho-debug]   /'
    echo "[osyntho-debug] ── sql drivers ───────────────────────────────────"
    ls -1 "\${APPDIR}/usr/plugins/sqldrivers/" 2>/dev/null | sed 's/^/[osyntho-debug]   /' || echo "[osyntho-debug]   NONE"
    echo "[osyntho-debug] ── xcb-cursor ────────────────────────────────────"
    ls -la "\${APPDIR}/usr/lib/libxcb-cursor"* 2>/dev/null | sed 's/^/[osyntho-debug]   /' || echo "[osyntho-debug]   NOT FOUND"
    echo "[osyntho-debug] ── launching with QT_DEBUG_PLUGINS=1 ─────────────"
    if [ "\${OSYNTHO_DEBUG_BLE:-0}" = "1" ]; then
        export QT_LOGGING_RULES="qt.bluetooth*=true"
    fi
    QT_DEBUG_PLUGINS=1 exec "\${APPDIR}/usr/bin/${APP_NAME}" "\$@"
fi

if [ "\${OSYNTHO_DEBUG_BLE:-0}" = "1" ]; then
    export QT_LOGGING_RULES="qt.bluetooth*=true"
fi

exec "\${APPDIR}/usr/bin/${APP_NAME}" "\$@"
APPRUN_EOF
chmod +x "$APPDIR/AppRun"
info "Wrote AppRun (LD_LIBRARY_PATH → AppDir/usr/lib)"

# Verify xcb-cursor and its transitive deps are all present before packing
if [ -f "$APPDIR/usr/lib/libxcb-cursor.so.0" ]; then
    info "Verified: libxcb-cursor.so.0 is in AppDir/usr/lib/"
    # Check that every xcb/X dep of xcb-cursor resolves inside AppDir or system
    MISSING_DEPS=()
    # NOTE: do NOT set IFS= here — that causes `read` to slurp the entire line
    # into $dep_name and leave $dep_path empty, so every dep looks unbundled.
    while read -r dep_name dep_path; do
        case "$dep_name" in
            libxcb*.so.*|libXau.so.*|libXdmcp.so.*|libxcb-util.so.*)
                if [ ! -f "$APPDIR/usr/lib/$dep_name" ]; then
                    MISSING_DEPS+=("$dep_name (system: $dep_path)")
                fi ;;
        esac
    done < <(ldd "$APPDIR/usr/lib/libxcb-cursor.so.0" 2>/dev/null \
             | awk '/=> \// { print $1, $3 }')
    if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
        warn "xcb-cursor transitive deps NOT bundled (relies on system):"
        for d in "${MISSING_DEPS[@]}"; do warn "  $d"; done
    else
        info "All xcb-cursor transitive deps are bundled."
    fi
else
    warn "libxcb-cursor.so.0 missing from AppDir/usr/lib — xcb plugin will fail at runtime"
fi

# Verify the app-specific Qt libraries survived. Each of these is a feature the
# app cannot degrade gracefully without: Bluetooth is the entire point of the
# companion app, Sql backs settings/presets/patch library, Network is used by
# the update/donation links.
step "Verifying app-specific Qt libraries..."
for _need in libQt6Bluetooth.so.6 libQt6Sql.so.6 libQt6Network.so.6 \
             libQt6Quick.so.6 libQt6QuickControls2.so.6; do
    if [ -f "$APPDIR/usr/lib/$_need" ]; then
        info "  present: $_need"
    else
        warn "  MISSING: $_need — the matching feature will fail at runtime"
    fi
done

# ── Step 3: appimagetool — pack final AppDir into AppImage ────────────────────
# appimagetool is a pure packer: it compresses whatever is in AppDir into a
# squashfs AppImage without any lib filtering. This is intentional — it lets
# us control the exact contents after linuxdeploy has finished.
step "Step 3/3: appimagetool (packing AppDir → AppImage)..."

APPIMAGE_FILE="${APP_FILE_BASE}-${APP_VERSION}-${ARCH}.AppImage"
APPIMAGE_PATH="$OUTPUT_DIR/$APPIMAGE_FILE"

APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$ARCH" "$APPIMAGETOOL" \
    --runtime-file "$APPIMAGE_RUNTIME" \
    "$APPDIR" "$APPIMAGE_PATH" \
    || die "appimagetool failed — check output above"

# ── Report ────────────────────────────────────────────────────────────────────
echo ""
if [ -f "$APPIMAGE_PATH" ]; then
    SIZE="$(du -sh "$APPIMAGE_PATH" | cut -f1)"
    info "Done!  AppImage: $APPIMAGE_PATH  ($SIZE)"
    echo ""
    echo "  Run:      $APPIMAGE_PATH"
    echo "  No FUSE:  $APPIMAGE_PATH --appimage-extract-and-run"
    echo "  Extract:  $APPIMAGE_PATH --appimage-extract  # produces squashfs-root/"
    echo "  Debug:    OSYNTHO_DEBUG=1 $APPIMAGE_PATH     # add OSYNTHO_DEBUG_BLE=1 for BLE logs"
    echo ""
    echo "  Runtime requirements on the target machine (NOT bundled — they are"
    echo "  system services, not libraries):"
    echo "    * BlueZ + a running bluetoothd — Qt Bluetooth talks to it over D-Bus."
    echo "      The user must also be allowed to scan (usually automatic on desktops)."
    echo "    * zenity — src/nativefiledialog.cpp shells out to it for the .olt/.wav"
    echo "      open/save dialogs. Without it those fall back to the Qt Quick dialog."
    echo "      sudo apt-get install zenity"
else
    warn "AppImage not found at expected path: $APPIMAGE_PATH"
    info "AppDir is still available. Test it directly with:"
    echo "  LD_LIBRARY_PATH=\"$APPDIR/usr/lib\" \"$APPDIR/usr/bin/$APP_NAME\""
fi
