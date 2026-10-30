#!/bin/bash
#
# Host-side regression tests for logic that is painful to verify on device.
#
# These do NOT test a copy of the code: each harness extracts the relevant
# functions verbatim from the plugin sources at run time and compiles them for
# the host, with the sce* calls shimmed to libc. If you change the extracted
# functions, the tests follow automatically; if you rename or restructure them,
# the extraction fails loudly rather than silently testing nothing.
#
# Covered:
#   statetest  - the state.txt read/write path in shell/impose.cpp and
#                companion/main.c: format round-trip, legacy cleanup, unknown
#                key preservation, refusal to truncate, shell->helper handoff
#   autodetect - the inject auto-detection state machine in companion/main.c
#                across realistic per-title frame patterns
#   btnmerge   - Antilag's syscon-press merge (companion/btnmerge.h): which
#                buttons may go early, release hiding, remapper/turbo/sticky
#                and injected-press behaviour
#
# Usage: bash tests/run.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

CXX=${CXX:-g++}
SAN="-fsanitize=address,undefined"
rc=0

printf '\n\033[1m--- compat table ---\033[0m\n'
if ! python3 "$ROOT/tools/gen_compat.py" --check; then rc=1; fi

extract_range() {   # file, start-regex -> stdout, up to the next line that is exactly "}"
    local f="$1" re="$2"
    local a e
    a=$(grep -n "$re" "$f" | head -1 | cut -d: -f1)
    if [ -z "$a" ]; then echo "EXTRACT FAILED: $re not found in $f" >&2; return 1; fi
    e=$(awk -v b="$a" 'NR>=b && /^}$/{print NR; exit}' "$f")
    if [ -z "$e" ]; then echo "EXTRACT FAILED: no end for $re in $f" >&2; return 1; fi
    sed -n "${a},${e}p" "$f"
}

run_one() {   # name
    local name="$1"
    printf '\n\033[1m--- %s ---\033[0m\n' "$name"
    # -I. so the generated .inc files (in $WORK) are found ahead of nothing;
    # the sources live in $ROOT/tests so their own directory is searched first.
    if ! $CXX -O1 $SAN -I. -o "$name" "$ROOT/tests/$name.cpp" 2>build.log; then
        echo "BUILD FAILED"; sed -n '1,25p' build.log; rc=1; return
    fi
    ./"$name" || rc=1
}

# --- statetest: state.txt read/write ---
{
    a=$(grep -n '^static char s_readBuf' "$ROOT/shell/impose.cpp" | cut -d: -f1)
    b=$(grep -n '^bool readGlobals' "$ROOT/shell/impose.cpp" | cut -d: -f1)
    e=$(awk -v b="$b" 'NR>=b && /^}$/{print NR; exit}' "$ROOT/shell/impose.cpp")
    if [ -z "$a" ] || [ -z "$e" ]; then echo "EXTRACT FAILED: shell state IO" >&2; exit 1; fi
    sed -n "${a},${e}p" "$ROOT/shell/impose.cpp"
} > shell_io.inc
{
    extract_range "$ROOT/companion/main.c" '^static int lineIsGlobalKey' || exit 1
    extract_range "$ROOT/companion/main.c" '^static int readFileInto' || exit 1
    extract_range "$ROOT/companion/main.c" '^static int readFileIntoBak' || exit 1
    extract_range "$ROOT/companion/main.c" '^static int antilagValue' || exit 1
    extract_range "$ROOT/companion/main.c" '^static void parseConfigLines' || exit 1
    extract_range "$ROOT/companion/main.c" '^static __attribute__((noinline)) void readGameConfig' || exit 1
} > comp_io.inc
run_one statetest

# --- autodetect: the inject state machine ---
extract_range "$ROOT/companion/main.c" '^static int autodetectTick' > autodetect.inc || exit 1
run_one autodetect

# --- flipwork: the CPU estimate for titles that present without AddEntry ---
extract_range "$ROOT/companion/main.c" '^static void noteFlipWork' > flipwork.inc || exit 1
run_one flipwork

# --- btnmerge: the syscon-press state machine (a header, included directly) ---
run_one btnmerge

# --- stkmerge: the early-sticks gate (a header, included directly) ---
run_one stkmerge

printf '\n'
if [ "$rc" -eq 0 ]; then echo "ALL SUITES PASSED"; else echo "FAILURES (see above)"; fi
exit $rc
