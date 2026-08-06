#!/bin/bash
# deploy_macos_multi.sh — Package the Osyntho companion app into a
# self-contained macOS .app bundle (and optionally a .dmg).
# Same arg shape as deploy_linux.sh.
#
# Usage:   ./deploy_macos_multi.sh <build_dir> [output_dir]
# Example: ./deploy_macos_multi.sh ~/dev/osynth_free_synth/app_osyntho/build/Qt_6_11_0_for_macOS-Release ~/dist
#
# <build_dir> is the CMake binary dir of app_osyntho/ (the Qt app), NOT of the
# repository root — the root CMakeLists.txt is the ESP-IDF firmware project.
#
# Auto-detects:
#   - Qt installation (from CMakeCache.txt in the build dir, then build-dir
#     name parsing, then `qmake` / `macdeployqt` on PATH)
#   - The .app bundle to deploy (anywhere up to 3 levels deep in build_dir)
#   - The QML source dir (used by macdeployqt to scan for QML imports)
#   - The architectures compiled into the app binary (via lipo). If the app is
#     universal (x86_64 + arm64), the Qt libraries are deployed universal too:
#       * If the detected Qt already ships universal frameworks (the official
#         Qt installer does), a single macdeployqt pass covers both archs.
#       * If Qt is thin per-arch (e.g. Homebrew), each arch is deployed against
#         its own Qt prefix and the results are lipo-merged into one bundle.
#     A single-arch app deploys exactly as before.
#
# NOTE: app_osyntho/CMakeLists.txt defaults CMAKE_OSX_ARCHITECTURES to "x86_64".
# For a universal artifact, configure the build with
#   -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
# otherwise this script correctly produces a single-arch bundle.
#
# Environment overrides:
#   QT_PREFIX_OVERRIDE          force the primary Qt prefix (e.g. ~/Qt/6.11.0/macos)
#   QT_PREFIX_X86_64_OVERRIDE   force the x86_64 Qt prefix for universal merges
#   QT_PREFIX_ARM64_OVERRIDE    force the arm64 Qt prefix for universal merges
#   MAKE_DMG=1                  also produce a .dmg next to the .app
#   QML_DIR_OVERRIDE            force a specific QML source dir for import scanning

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The Qt app lives in app_osyntho/, while this script sits at the repository
# root (next to the firmware's CMakeLists.txt). Everything source-related —
# version, qml/, assets/ — must be read from the app dir. Falls back to
# SCRIPT_DIR so the script keeps working if it is ever moved into app_osyntho/.
if [ -d "$SCRIPT_DIR/app_osyntho" ]; then
    APP_SRC_DIR="$SCRIPT_DIR/app_osyntho"
else
    APP_SRC_DIR="$SCRIPT_DIR"
fi

# Name of the built executable = the CMake project()/target name. It stays
# lowercase inside Contents/MacOS/ even though the bundle itself is renamed to
# the display name below (macOS resolves it through CFBundleExecutable).
APP_NAME="osyntho"
# Store/display name: the label users actually see (Android label, macOS
# CFBundleDisplayName). APP_FILE_BASE is the same identity without spaces, for
# artifact filenames — and here also for the .app bundle directory name.
APP_DISPLAY_NAME="Osyntho"
APP_FILE_BASE="Osyntho"
# Must match APP_ID in app_osyntho/CMakeLists.txt and CFBundleIdentifier in
# assets/macos/Info.plist.
APP_ID="org.osynth.osyntho"
# Fallback only — overridden from the built bundle's Info.plist below.
# Read from CMakeLists.txt rather than repeated here: a hard-coded copy drifts
# from the app within a release or two, and then every artifact this script
# produces is stamped with a version the app never had.
APP_VERSION="$(sed -nE 's/^[[:space:]]*set\(APP_VERSION[[:space:]]+([0-9][0-9.]*)\).*/\1/p' "$APP_SRC_DIR/CMakeLists.txt" 2>/dev/null | head -1)"
APP_VERSION="${APP_VERSION:-0.0.0}"

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; NC='\033[0m'
die()  { echo -e "${RED}ERROR: $*${NC}" >&2; exit 1; }
info() { echo -e "${GREEN}[deploy] $*${NC}"; }
warn() { echo -e "${YELLOW}[deploy] WARNING: $*${NC}"; }
step() { echo -e "${CYAN}[deploy] >>> $*${NC}"; }

# ── Mach-O architecture helpers ───────────────────────────────────────────────
file_archs()    { lipo -archs "$1" 2>/dev/null || true; }          # space-sep archs, or empty if not Mach-O
contains_arch() { case " $2 " in *" $1 "*) return 0;; *) return 1;; esac; }  # contains_arch <arch> <list>

# ── Info.plist reader ─────────────────────────────────────────────────────────
plist_value() {  # plist_value <key> <plist>; echoes the value, or nothing
    local key="$1" plist="$2"
    [ -f "$plist" ] || return 0
    if [ -x /usr/libexec/PlistBuddy ]; then
        /usr/libexec/PlistBuddy -c "Print :$key" "$plist" 2>/dev/null || true
    else
        defaults read "${plist%.plist}" "$key" 2>/dev/null || true   # path without .plist
    fi
}

# ── Arguments ─────────────────────────────────────────────────────────────────
BUILD_DIR="${1:-}"
OUTPUT_DIR="${2:-$SCRIPT_DIR/dist-macos}"

[ -n "$BUILD_DIR" ] || die "Usage: $0 <build_dir> [output_dir]\nExample: $0 ~/build/Qt_6_11_0_for_macOS-Release ~/dist"
[ -d "$BUILD_DIR" ] || die "Build directory not found: $BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

# Sanity: only run on macOS (or at least warn loudly elsewhere — macdeployqt
# is macOS-only).
if [ "$(uname)" != "Darwin" ]; then
    warn "This script is for macOS — uname reports $(uname). Continuing anyway."
fi

# ── Locate the .app bundle inside the build dir ───────────────────────────────
step "Locating .app bundle..."
APP_BUNDLE=""
# Most builds put the .app at the build root; some put it under a subdir.
for cand in \
    "$BUILD_DIR/$APP_NAME.app" \
    "$BUILD_DIR/Release/$APP_NAME.app" \
    "$BUILD_DIR/Debug/$APP_NAME.app" \
    "$BUILD_DIR/bin/$APP_NAME.app"; do
    if [ -d "$cand" ]; then APP_BUNDLE="$cand"; break; fi
done
if [ -z "$APP_BUNDLE" ]; then
    APP_BUNDLE="$(find "$BUILD_DIR" -maxdepth 3 -name "$APP_NAME.app" -type d 2>/dev/null | head -1)"
fi
[ -n "$APP_BUNDLE" ] && [ -d "$APP_BUNDLE" ] || \
    die "Could not find $APP_NAME.app under $BUILD_DIR. Build app_osyntho/ first."

# Verify the inner executable exists (catches half-built / corrupted bundles).
if [ ! -f "$APP_BUNDLE/Contents/MacOS/$APP_NAME" ]; then
    die ".app bundle is missing its executable: $APP_BUNDLE/Contents/MacOS/$APP_NAME"
fi

# ── App version (read from the built bundle's Info.plist) ──────────────────────
# CMake writes the project VERSION into the bundle: CFBundleVersion is the full
# version (e.g. 0.1.5); CFBundleShortVersionString is major.minor (e.g. 0.1).
# Prefer the full one; fall back to the short one, then to the literal above.
INFO_PLIST="$APP_BUNDLE/Contents/Info.plist"
DETECTED_VERSION="$(plist_value CFBundleVersion "$INFO_PLIST")"
[ -n "$DETECTED_VERSION" ] || DETECTED_VERSION="$(plist_value CFBundleShortVersionString "$INFO_PLIST")"
if [ -n "$DETECTED_VERSION" ]; then
    APP_VERSION="$DETECTED_VERSION"
    VERSION_SOURCE="Info.plist"
else
    VERSION_SOURCE="default (Info.plist had no version)"
fi

# Identity check: assets/macos/Info.plist is a hand-maintained template, so it
# is the one file that can silently ship the wrong app identity (it started life
# as a copy from another project). Catch that here rather than in the App Store
# / on a user's machine.
DETECTED_ID="$(plist_value CFBundleIdentifier "$INFO_PLIST")"
if [ -n "$DETECTED_ID" ] && [ "$DETECTED_ID" != "$APP_ID" ]; then
    warn "Bundle identifier is '$DETECTED_ID', expected '$APP_ID'."
    warn "  Fix CFBundleIdentifier in app_osyntho/assets/macos/Info.plist and rebuild."
fi

ARCH="$(uname -m)"
APP_EXE="$APP_BUNDLE/Contents/MacOS/$APP_NAME"

# Architectures actually compiled into the app binary (e.g. "x86_64 arm64").
APP_ARCHS=""
if command -v lipo &>/dev/null; then
    APP_ARCHS="$(file_archs "$APP_EXE")"
fi
[ -n "$APP_ARCHS" ] || APP_ARCHS="$ARCH"   # fallback if lipo missing / not Mach-O
UNIVERSAL=0
if contains_arch x86_64 "$APP_ARCHS" && contains_arch arm64 "$APP_ARCHS"; then
    UNIVERSAL=1
fi

# Label used in artifact filenames. Derived from what the binary actually
# contains, not from `uname -m`: a cross-built or universal bundle produced on
# an arm64 Mac must not be named "…-arm64".
if [ "$UNIVERSAL" = 1 ]; then
    ARCH_LABEL="universal"
else
    ARCH_LABEL="$(echo "$APP_ARCHS" | awk '{print $1}')"
fi
[ -n "$ARCH_LABEL" ] || ARCH_LABEL="$ARCH"

info "App          : $APP_DISPLAY_NAME"
info "Host arch    : $ARCH"
if [ "$UNIVERSAL" = 1 ]; then
    info "App archs    : $APP_ARCHS  (universal)"
else
    info "App archs    : $APP_ARCHS"
fi
info "App version  : $APP_VERSION ($VERSION_SOURCE)"
info "App source   : $APP_SRC_DIR"
info "Build dir    : $BUILD_DIR"
info "Output dir   : $OUTPUT_DIR"
info "App bundle   : $APP_BUNDLE"

# ── Detect Qt installation ────────────────────────────────────────────────────
step "Detecting Qt installation..."

QT_PREFIX=""
QMAKE_BIN=""

# 1. Explicit override
if [ -n "${QT_PREFIX_OVERRIDE:-}" ]; then
    if [ -x "$QT_PREFIX_OVERRIDE/bin/qmake" ] || [ -x "$QT_PREFIX_OVERRIDE/bin/qmake6" ]; then
        QT_PREFIX="$QT_PREFIX_OVERRIDE"
        info "Using QT_PREFIX_OVERRIDE : $QT_PREFIX"
    else
        die "QT_PREFIX_OVERRIDE='$QT_PREFIX_OVERRIDE' has no bin/qmake"
    fi
fi

# 2. CMakeCache.txt — tells us exactly which Qt the build was linked against.
if [ -z "$QT_PREFIX" ] && [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    RAW="$(awk -F'=' '/^Qt6_DIR:PATH=/{print $2; exit}' "$BUILD_DIR/CMakeCache.txt")"
    # RAW e.g. /Users/foo/Qt/6.11.0/macos/lib/cmake/Qt6
    if [ -n "$RAW" ]; then
        CAND="${RAW%/lib/cmake/Qt6}"
        if [ -x "$CAND/bin/qmake" ] || [ -x "$CAND/bin/qmake6" ]; then
            QT_PREFIX="$CAND"
            info "Detected via CMakeCache.txt : $QT_PREFIX"
        fi
    fi
fi

# 3. Parse the build dir name (Qt Creator's default layout).
#    e.g. Qt_6_11_0_for_macOS-Release  →  6.11.0
if [ -z "$QT_PREFIX" ]; then
    BUILD_DIR_NAME="$(basename "$BUILD_DIR")"
    PARSED_VER="$(echo "$BUILD_DIR_NAME" | sed -nE 's/.*Qt_([0-9]+)_([0-9]+)_([0-9]+).*/\1.\2.\3/p')"
    if [ -n "$PARSED_VER" ]; then
        for ROOT in "$HOME/Qt" "/opt/Qt" "/Users/Shared/Qt"; do
            for SUB in macos clang_64; do
                CAND="$ROOT/$PARSED_VER/$SUB"
                if [ -x "$CAND/bin/qmake" ] || [ -x "$CAND/bin/qmake6" ]; then
                    QT_PREFIX="$CAND"
                    info "Detected via build-dir name : $QT_PREFIX (Qt $PARSED_VER)"
                    break 2
                fi
            done
        done
    fi
fi

# 4. macdeployqt or qmake already on PATH?
if [ -z "$QT_PREFIX" ]; then
    for tool in qmake6 qmake macdeployqt; do
        if command -v "$tool" &>/dev/null; then
            QT_PREFIX="$(cd "$(dirname "$(command -v "$tool")")/.." && pwd)"
            info "Detected via PATH ($tool) : $QT_PREFIX"
            break
        fi
    done
fi

# 5. Homebrew layouts (last-resort)
if [ -z "$QT_PREFIX" ]; then
    for BREW_PREFIX in /opt/homebrew/opt/qt /opt/homebrew/opt/qt@6 /usr/local/opt/qt /usr/local/opt/qt@6; do
        if [ -x "$BREW_PREFIX/bin/qmake" ] || [ -x "$BREW_PREFIX/bin/qmake6" ]; then
            QT_PREFIX="$BREW_PREFIX"
            info "Detected via Homebrew : $QT_PREFIX"
            break
        fi
    done
fi

[ -n "$QT_PREFIX" ] || die "Could not detect Qt installation. Set QT_PREFIX_OVERRIDE=/path/to/Qt/<version>/macos"

# Pick the matching qmake & verify macdeployqt exists
for q in "$QT_PREFIX/bin/qmake6" "$QT_PREFIX/bin/qmake"; do
    [ -x "$q" ] && { QMAKE_BIN="$q"; break; }
done
[ -n "$QMAKE_BIN" ] || die "$QT_PREFIX/bin has no qmake/qmake6"

MACDEPLOYQT="$QT_PREFIX/bin/macdeployqt"
[ -x "$MACDEPLOYQT" ] || die "macdeployqt not found at $MACDEPLOYQT"

QT_VERSION="$("$QMAKE_BIN" -query QT_VERSION 2>/dev/null || true)"
info "Qt prefix    : $QT_PREFIX"
info "Qt version   : ${QT_VERSION:-unknown}"
info "macdeployqt  : $MACDEPLOYQT"

# Put Qt at the front of PATH so any tool macdeployqt shells out to picks
# up our Qt version, not a stale one. Keep the pre-Qt PATH so per-arch
# macdeployqt runs (universal merge) can each prepend their own Qt prefix.
PATH_BASE="$PATH"
export PATH="$QT_PREFIX/bin:$PATH_BASE"

# ── Universal (multi-arch) strategy ───────────────────────────────────────────
# If the app binary is universal we want the deployed Qt libraries universal too.
# Official Qt-installer frameworks already are (single macdeployqt pass covers
# both archs); thin installs (e.g. Homebrew, one arch each) are not, so we deploy
# each arch against its own Qt prefix and lipo-merge the results into one bundle.
DEPLOY_MODE="single"     # "single" | "merge"
PREFIX_x86_64=""
PREFIX_arm64=""

# Echo a Qt prefix whose QtCore framework contains <arch>, or nothing. Honours
# QT_PREFIX_X86_64_OVERRIDE / QT_PREFIX_ARM64_OVERRIDE, then the primary prefix,
# then common Homebrew and Qt-installer layouts.
find_qt_prefix_for_arch() {
    local arch="$1" cand qtcore oval=""
    local candidates=()
    case "$arch" in
        x86_64) oval="${QT_PREFIX_X86_64_OVERRIDE:-}";;
        arm64)  oval="${QT_PREFIX_ARM64_OVERRIDE:-}";;
    esac
    [ -n "$oval" ] && candidates+=("$oval")
    candidates+=("$QT_PREFIX")
    if [ "$arch" = "arm64" ]; then
        candidates+=(/opt/homebrew/opt/qt /opt/homebrew/opt/qt@6 /opt/homebrew)
    else
        candidates+=(/usr/local/opt/qt /usr/local/opt/qt@6 /usr/local)
    fi
    [ -n "$QT_VERSION" ] && candidates+=("$HOME/Qt/$QT_VERSION/macos")
    for cand in "${candidates[@]}"; do
        qtcore="$cand/lib/QtCore.framework/Versions/A/QtCore"
        # Need both: a QtCore slice for this arch AND a macdeployqt to run.
        if [ -f "$qtcore" ] && [ -x "$cand/bin/macdeployqt" ] \
           && contains_arch "$arch" "$(file_archs "$qtcore")"; then
            echo "$cand"; return 0
        fi
    done
    return 0
}

if [ "$UNIVERSAL" = 1 ]; then
    step "Resolving universal Qt sources..."
    PREFIX_x86_64="$(find_qt_prefix_for_arch x86_64)"
    PREFIX_arm64="$(find_qt_prefix_for_arch arm64)"
    info "Qt for x86_64 : ${PREFIX_x86_64:-<not found>}"
    info "Qt for arm64  : ${PREFIX_arm64:-<not found>}"
    if [ -n "$PREFIX_x86_64" ] && [ -n "$PREFIX_arm64" ]; then
        if [ "$PREFIX_x86_64" = "$PREFIX_arm64" ]; then
            info "Primary Qt is universal — a single macdeployqt pass covers both archs."
        else
            DEPLOY_MODE="merge"
            info "Thin per-arch Qt detected — will deploy each arch and lipo-merge."
        fi
    else
        warn "Could not find a Qt providing every app arch — deploying with the primary Qt only."
        warn "  Bundle may end up thin; set QT_PREFIX_X86_64_OVERRIDE / QT_PREFIX_ARM64_OVERRIDE to fix."
    fi
fi

# ── Locate the QML source dir ─────────────────────────────────────────────────
step "Locating QML source dir..."
QML_DIR=""
if [ -n "${QML_DIR_OVERRIDE:-}" ]; then
    [ -d "$QML_DIR_OVERRIDE" ] || die "QML_DIR_OVERRIDE='$QML_DIR_OVERRIDE' is not a directory"
    QML_DIR="$QML_DIR_OVERRIDE"
else
    for cand in \
        "$APP_SRC_DIR/qml" \
        "$BUILD_DIR/../qml" \
        "$BUILD_DIR/../../qml"; do
        if [ -d "$cand" ]; then
            QML_DIR="$(cd "$cand" && pwd)"
            break
        fi
    done
fi
if [ -n "$QML_DIR" ]; then
    info "QML source dir : $QML_DIR"
else
    warn "No qml/ source dir found — macdeployqt will only scan the binary."
    warn "  QtQuick.Controls.Material and QtQuick.Dialogs are imported only from QML,"
    warn "  so the deployed bundle will very likely fail to load Main.qml."
fi

# ── Prepare output dir ────────────────────────────────────────────────────────
step "Preparing output directory..."
# The bundle is named after the display name (Osyntho.app), not the lowercase
# CMake target — that name is what Finder, the Dock and the DMG window show.
# macOS finds the inner binary through CFBundleExecutable, so the two may differ.
DEST_APP="$OUTPUT_DIR/$APP_FILE_BASE.app"
DMG_FINAL="$OUTPUT_DIR/$APP_FILE_BASE-$APP_VERSION-$ARCH_LABEL.dmg"
# macdeployqt -dmg names its output after the bundle, so that intermediate name
# is what has to be cleaned up (and later renamed) as well.
DMG_STAGED="$OUTPUT_DIR/$APP_FILE_BASE.dmg"
if [ -d "$DEST_APP" ]; then
    info "Removing previous $DEST_APP"
    rm -rf "$DEST_APP"
fi
# Also remove stale .app/.dmg from prior runs, including ones made before the
# bundle was renamed from the target name to the display name.
rm -rf "$OUTPUT_DIR/$APP_NAME.app" 2>/dev/null || true
rm -f "$DMG_FINAL" "$DMG_STAGED" "$OUTPUT_DIR/$APP_NAME.dmg" 2>/dev/null || true

# ── Copy bundle to output dir (so we don't mutate the build tree) ─────────────
step "Copying .app bundle to output dir..."
# cp -R follows symlinks by default, which can break frameworks. -a preserves
# symlinks and metadata — the correct mode for .app bundles on macOS.
cp -a "$APP_BUNDLE" "$DEST_APP"
info "Copied: $DEST_APP"

# ── Run macdeployqt ───────────────────────────────────────────────────────────
# Common macdeployqt args shared by every invocation (the bundle is added per call).
# -verbose=1 emits useful but not overwhelming output.
MACDEPLOY_COMMON=()
[ -n "$QML_DIR" ] && MACDEPLOY_COMMON+=("-qmldir=$QML_DIR")
MACDEPLOY_COMMON+=("-verbose=1")

# run_macdeployqt <bundle> <qt_prefix> [extra args...]
# Deploys <bundle> using the macdeployqt from <qt_prefix>, with that prefix at the
# front of PATH so its helper tools (qmlimportscanner, …) match the Qt version.
run_macdeployqt() {
    local bundle="$1" prefix="$2"; shift 2
    local args=("$bundle" "${MACDEPLOY_COMMON[@]}")
    # If a sqldrivers/ dir was staged inside the bundle, point macdeployqt at it
    # so it picks up libqsqlite.
    local srcsql="$bundle/Contents/PlugIns/sqldrivers"
    if [ -d "$srcsql" ]; then
        args+=("-libpath=$srcsql")
        info "Including sqldrivers libpath: $srcsql"
    fi
    args+=("$@")
    info "macdeployqt [$(basename "$prefix")] -> $bundle"
    PATH="$prefix/bin:$PATH_BASE" "$prefix/bin/macdeployqt" "${args[@]}" \
        || die "macdeployqt failed for $bundle"
}

if [ "$DEPLOY_MODE" = "merge" ]; then
    step "Deploying universal bundle (per-arch macdeployqt + lipo merge)..."
    TMP_MERGE="$(mktemp -d "${TMPDIR:-/tmp}/osyntho-univ.XXXXXX")"
    trap 'rm -rf "$TMP_MERGE"' EXIT
    MERGE_BUNDLES=()
    for a in $APP_ARCHS; do
        case "$a" in
            x86_64) pfx="$PREFIX_x86_64";;
            arm64)  pfx="$PREFIX_arm64";;
            *) warn "No Qt prefix mapped for arch '$a' — skipping"; continue;;
        esac
        b="$TMP_MERGE/$a/$APP_FILE_BASE.app"
        mkdir -p "$TMP_MERGE/$a"
        cp -a "$DEST_APP" "$b"
        # Thin the main executable so macdeployqt deploys the matching Qt arch.
        exe="$b/Contents/MacOS/$APP_NAME"
        lipo "$exe" -thin "$a" -output "$exe.__thin" && mv -f "$exe.__thin" "$exe"
        run_macdeployqt "$b" "$pfx"
        MERGE_BUNDLES+=("$b")
    done

    [ "${#MERGE_BUNDLES[@]}" -ge 1 ] || die "Universal merge produced no per-arch bundles"

    step "Merging per-arch bundles with lipo..."
    # Rebuild DEST_APP from the first per-arch bundle, then fatten every Mach-O
    # file by lipo-creating it from the matching files in all per-arch bundles.
    BASE_BUNDLE="${MERGE_BUNDLES[0]}"
    rm -rf "$DEST_APP"
    cp -a "$BASE_BUNDLE" "$DEST_APP"
    MERGED=0
    while IFS= read -r -d '' f; do
        base_archs="$(file_archs "$f")"
        [ -n "$base_archs" ] || continue            # not a Mach-O file
        rel="${f#"$DEST_APP"/}"
        need=0
        for a in $APP_ARCHS; do
            contains_arch "$a" "$base_archs" || need=1
        done
        [ "$need" = 0 ] && continue                 # already fat
        inputs=()
        for b in "${MERGE_BUNDLES[@]}"; do
            cand="$b/$rel"
            [ -f "$cand" ] && [ -n "$(file_archs "$cand")" ] && inputs+=("$cand")
        done
        if [ "${#inputs[@]}" -ge 2 ]; then
            if lipo -create "${inputs[@]}" -output "$f" 2>/dev/null; then
                MERGED=$((MERGED + 1))
            else
                warn "  lipo merge failed for $rel — leaving as-is"
            fi
        fi
    done < <(find "$DEST_APP" -type f -print0)
    info "Fattened $MERGED Mach-O file(s) into the universal bundle."
    rm -rf "$TMP_MERGE"
    trap - EXIT
else
    step "Running macdeployqt..."
    SINGLE_EXTRA=()
    [ "${MAKE_DMG:-0}" = "1" ] && SINGLE_EXTRA+=("-dmg")   # macdeployqt builds the DMG
    run_macdeployqt "$DEST_APP" "$QT_PREFIX" "${SINGLE_EXTRA[@]}"
fi

# ── SQLite driver ─────────────────────────────────────────────────────────────
# The app opens its settings/presets/patch-library database through
# QSqlDatabase::addDatabase("QSQLITE"), so libqsqlite.dylib is not optional:
# without it Database::open() fails the isDriverAvailable() check and the app
# starts with no settings at all. macdeployqt usually copies it because QtSql is
# linked, but make sure — and drop the client-library-dependent drivers it may
# have copied alongside (libpq/libmysqlclient/unixodbc are not on user machines,
# and unresolved dylibs upset codesign).
step "Checking SQL drivers..."
SQLDRIVERS_DIR="$DEST_APP/Contents/PlugIns/sqldrivers"
if [ ! -f "$SQLDRIVERS_DIR/libqsqlite.dylib" ]; then
    if [ -f "$QT_PREFIX/plugins/sqldrivers/libqsqlite.dylib" ]; then
        mkdir -p "$SQLDRIVERS_DIR"
        cp "$QT_PREFIX/plugins/sqldrivers/libqsqlite.dylib" "$SQLDRIVERS_DIR/"
        info "Copied libqsqlite.dylib (macdeployqt had not)"
    else
        warn "libqsqlite.dylib not found in $QT_PREFIX/plugins/sqldrivers/ — SQLite will fail at runtime"
    fi
else
    info "libqsqlite.dylib present"
fi
for DRV in libqsqlibase.dylib libqsqlodbc.dylib libqsqlpsql.dylib libqsqlmysql.dylib; do
    if [ -f "$SQLDRIVERS_DIR/$DRV" ]; then
        rm -f "$SQLDRIVERS_DIR/$DRV"
        info "Removed unused driver: $DRV"
    fi
done

# ── QML module dir ────────────────────────────────────────────────────────────
# app_osyntho/CMakeLists.txt copies an empty assets/qmldir to
# <build>/org/osynth/qmldir because the QML engine looks for that path next to
# the executable when resolving the org.osynth.osyntho module. Mirror it inside
# the bundle so a deployed .app behaves like the build tree.
QMLDIR_SRC=""
for cand in "$BUILD_DIR/org/osynth/qmldir" "$APP_BUNDLE/Contents/MacOS/org/osynth/qmldir" \
            "$APP_SRC_DIR/assets/qmldir"; do
    if [ -f "$cand" ]; then QMLDIR_SRC="$cand"; break; fi
done
if [ -n "$QMLDIR_SRC" ]; then
    mkdir -p "$DEST_APP/Contents/MacOS/org/osynth"
    cp "$QMLDIR_SRC" "$DEST_APP/Contents/MacOS/org/osynth/qmldir"
    info "Copied org/osynth/qmldir (from $QMLDIR_SRC)"
else
    warn "No org/osynth/qmldir found — QML module resolution may fail"
fi

# ── Copy assets ───────────────────────────────────────────────────────────────
# Osyntho loads every asset from the compiled-in resources (:/assets/... via
# main_resources.qrc), so this is belt-and-braces for the graph.svg/graph.ico
# copies the CMake POST_BUILD step drops next to the binary — a few KB.
SRC_ASSETS=""
for cand in "$BUILD_DIR/assets" "$APP_BUNDLE/Contents/MacOS/assets"; do
    if [ -d "$cand" ]; then SRC_ASSETS="$cand"; break; fi
done
if [ -n "$SRC_ASSETS" ]; then
    step "Copying assets/ into bundle..."
    mkdir -p "$DEST_APP/Contents/Resources/assets"
    cp -a "$SRC_ASSETS"/. "$DEST_APP/Contents/Resources/assets/"
    info "Copied: $SRC_ASSETS → $DEST_APP/Contents/Resources/assets/"
else
    info "No assets/ dir found alongside the build — skipping"
fi

# ── Verify deployment ─────────────────────────────────────────────────────────
step "Verifying deployment..."
MISSING=0
# QtBluetooth and QtSql are on this list because they are what makes this a
# companion app: no Bluetooth framework means it can never reach the synth, no
# Sql means no settings/presets. The rest are the Qt Quick baseline.
for fw in QtCore QtGui QtQml QtQuick QtQuickControls2 QtNetwork QtBluetooth QtSql; do
    if [ ! -d "$DEST_APP/Contents/Frameworks/$fw.framework" ]; then
        warn "  Missing framework: $fw.framework"
        MISSING=$((MISSING + 1))
    fi
done
if [ ! -f "$DEST_APP/Contents/PlugIns/platforms/libqcocoa.dylib" ]; then
    warn "  Missing platform plugin: libqcocoa.dylib"
    MISSING=$((MISSING + 1))
fi
# The Material style is imported by 36 of the QML files; if the qmldir scan
# missed it the app launches to a blank window instead of an error.
if [ ! -d "$DEST_APP/Contents/Resources/qml/QtQuick/Controls/Material" ]; then
    warn "  Missing QML module: QtQuick.Controls.Material (check -qmldir scanning)"
    MISSING=$((MISSING + 1))
fi
if [ "$MISSING" -gt 0 ]; then
    warn "$MISSING expected file(s) missing — macdeployqt may have only partially succeeded."
else
    info "All critical frameworks, plugins and QML modules present."
fi

# Architecture coverage: confirm the deployed Qt frameworks carry every arch the
# app binary does (the whole point of the universal deploy).
FINAL_EXE_ARCHS="$(file_archs "$DEST_APP/Contents/MacOS/$APP_NAME")"
info "Final binary archs : ${FINAL_EXE_ARCHS:-unknown}"
if [ "$UNIVERSAL" = 1 ]; then
    THIN_FW=0
    for fw in QtCore QtGui QtQml QtQuick QtNetwork QtBluetooth QtSql; do
        bin="$DEST_APP/Contents/Frameworks/$fw.framework/Versions/A/$fw"
        [ -f "$bin" ] || continue
        fwa="$(file_archs "$bin")"
        for a in $APP_ARCHS; do
            if ! contains_arch "$a" "$fwa"; then
                warn "  $fw.framework missing arch '$a' (has: ${fwa:-none})"
                THIN_FW=$((THIN_FW + 1))
            fi
        done
    done
    if [ "$THIN_FW" = 0 ]; then
        info "Deployed Qt frameworks are universal ($APP_ARCHS)."
    else
        warn "$THIN_FW framework/arch combination(s) missing — bundle is NOT fully universal."
    fi
fi

# Check the binary doesn't have unresolved @rpath references to the build tree.
BAD_LINKS="$(otool -L "$DEST_APP/Contents/MacOS/$APP_NAME" 2>/dev/null \
              | awk '/^\t/ { print $1 }' \
              | grep -E '^/Users/|^/Volumes/|^/Library/Frameworks/' \
              | grep -vE '^/usr/lib/|^/System/' \
              || true)"
if [ -n "$BAD_LINKS" ]; then
    warn "Binary still references absolute paths outside the bundle:"
    echo "$BAD_LINKS" | sed 's/^/  /' >&2
    warn "  Users without Qt installed at those paths will fail to launch."
fi

# ── Code signing (ad-hoc, just so Gatekeeper stops complaining locally) ───────
# Real notarisation needs a Developer ID; this only does ad-hoc signing so the
# bundle can be moved around and launched on the build machine.
# It matters more here than for a plain app: macOS only shows the Bluetooth
# permission prompt for a signed bundle carrying NSBluetoothAlwaysUsageDescription
# (set in app_osyntho/assets/macos/Info.plist). Unsigned, the first BLE scan can
# fail silently with no prompt at all.
if command -v codesign &>/dev/null; then
    step "Ad-hoc signing the bundle (no notarisation)..."
    codesign --force --deep --sign - "$DEST_APP" 2>&1 | sed 's/^/[deploy]   /' || \
        warn "codesign reported errors — the bundle still works locally but Gatekeeper may complain on other machines."
else
    warn "codesign not found — skipping ad-hoc signing"
fi

# ── DMG ───────────────────────────────────────────────────────────────────────
# In single mode macdeployqt already produced the DMG via -dmg, named after the
# bundle. The merge path ran macdeployqt per-arch on temp bundles, so build the
# DMG from the final (signed) universal bundle here. Either way the artifact is
# renamed to Osyntho-<version>-<arch>.dmg to match the Linux AppImage naming.
if [ "${MAKE_DMG:-0}" = "1" ]; then
    if [ "$DEPLOY_MODE" = "merge" ] || [ ! -f "$DMG_STAGED" ]; then
        step "Creating DMG (hdiutil)..."
        rm -f "$DMG_STAGED"
        if hdiutil create -volname "$APP_DISPLAY_NAME" -srcfolder "$DEST_APP" \
                -ov -format UDZO "$DMG_STAGED" >/dev/null; then
            info "Created: $DMG_STAGED"
        else
            warn "hdiutil failed to create DMG"
        fi
    fi
    if [ -f "$DMG_STAGED" ]; then
        mv -f "$DMG_STAGED" "$DMG_FINAL"
        info "DMG: $DMG_FINAL"
    fi
fi

# ── Report ────────────────────────────────────────────────────────────────────
echo ""
SIZE="$(du -sh "$DEST_APP" | cut -f1)"
info "Done!  $DEST_APP  ($SIZE)"
if [ "${MAKE_DMG:-0}" = "1" ] && [ -f "$DMG_FINAL" ]; then
    DMG_SIZE="$(du -sh "$DMG_FINAL" | cut -f1)"
    info "       $DMG_FINAL  ($DMG_SIZE)"
fi
echo ""
echo "  Open:    open '$DEST_APP'"
echo "  Run:     '$DEST_APP/Contents/MacOS/$APP_NAME'"
echo "  BLE log: QT_LOGGING_RULES='qt.bluetooth*=true' '$DEST_APP/Contents/MacOS/$APP_NAME'"
if [ "${MAKE_DMG:-0}" != "1" ]; then
    echo "  DMG:     MAKE_DMG=1 $0 $BUILD_DIR $OUTPUT_DIR"
fi
echo ""
echo "  First launch asks for Bluetooth access (System Settings ▸ Privacy &"
echo "  Security ▸ Bluetooth). Denying it leaves the synth permanently"
echo "  undiscoverable — re-enable it there, not by reinstalling."
