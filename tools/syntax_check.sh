#!/usr/bin/env bash
# Parse-and-typecheck sources with the exact flags the last build used, with
# no object emitted and nothing linked (-fsyntax-only). A static check, not a
# build.
#
# It replays the "command" entry from build/compile_commands.json, which is the
# only way to validate an edit against the real include tree, the real
# sdkconfig and the real -Wall -Werror set. Worth running after touching a
# widely-included header, before handing the tree to a build.
#
#   tools/syntax_check.sh synth_params.cpp fx.cpp midi.c ...
#   tools/syntax_check.sh --changed        (every .c/.cpp git reports modified)
#
# The JSON carries Windows paths as escaped backslashes and wraps the response
# file in escaped quotes. Escaped quotes are dropped and backslashes become
# '/', which is safe here only because none of these paths contain spaces;
# -o/-c/-DIDF_VER are stripped first, since their arguments carry quoting that
# would not survive that. Field splitting uses parameter expansion rather than
# a regex: the command string contains both quotes and commas of its own.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="$ROOT/build/compile_commands.json"
[ -f "$CC" ] || { echo "no compile_commands.json — configure the build once"; exit 1; }

if [ "${1:-}" = "--changed" ]; then
  set -- $(cd "$ROOT" && git diff --name-only | grep -E '\.(c|cpp)$' | xargs -r -n1 basename)
  [ $# -gt 0 ] || { echo "no modified .c/.cpp files"; exit 0; }
fi

FLAT="$(mktemp)"
tr -d '\n' < "$CC" | sed 's/},[[:space:]]*{/}\n{/g' > "$FLAT"

norm() { printf '%s' "$1" | tr '\' '/' | sed 's|//*|/|g'; }

rc=0
for base in "$@"; do
  entry=$(grep -F "\\$base\"," "$FLAT" | head -1)
  [ -n "$entry" ] || { echo "SKIP  $base (not in compile_commands.json)"; continue; }

  rest=${entry#*\"command\": \"}
  raw=${rest%%\",*\"file\"*}
  [ "$raw" != "$rest" ] || { echo "SKIP  $base (could not split the entry)"; continue; }

  rest=${entry#*\"file\": \"}
  src=$(norm "${rest%%\"*}")

  raw=$(printf '%s' "$raw" | sed 's/ -o [^ ]*//; s/ -c [^ ]*//; s/ -DIDF_VER=[^ ]*//; s/\\"//g')
  cmd=$(norm "$raw")

  echo "### $base"
  if eval "$cmd -DIDF_VER='\"syntax-check\"' -fsyntax-only '$src'"; then
    echo "     ok"
  else
    rc=1
  fi
done
rm -f "$FLAT"
exit $rc
