#!/usr/bin/env bash
# How much of the code the tests reach, measured with Clang's source-based coverage. Run it from anywhere in
# the repository, on macOS, or on Linux with Clang and LLVM's llvm-profdata and llvm-cov:
#
#     scripts/coverage.sh                  builds the coverage preset, runs every test, and prints the share
#                                          of lines and functions they reach in each library, app and the
#                                          examples, and in all of them
#     CXX=clang++ scripts/coverage.sh      the same on Linux, where the compiler is otherwise GCC
#
# The table is kept in build/coverage/summary.txt, the report file by file in build/coverage/report.txt, and
# one to browse line by line in build/coverage/html/index.html. The tests' own code, generated code and other
# projects' headers are left out.
# It stops if a test fails: coverage from a failing run says little.
set -uo pipefail

# From the script's own place, not git: CI's Ubuntu job in Docker builds a copy without .git.
repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo" || exit 2
build="$repo/build/coverage"
profiles="$build/profiles" # where the coverage test preset's LLVM_PROFILE_FILE puts each program's counts
profdata="$build/coverage.profdata"

usage() {
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-}" in
"") ;;
-h | --help)
    usage
    exit 0
    ;;
*)
    usage >&2
    exit 2
    ;;
esac

# The folders reported one by one; together they are everything but the tests.
readonly kAreas=(libs/visual_debug libs/viewer_core libs/frame_io libs/lab_core apps/qt_common apps/viewer
    apps/example_sender apps/document_lab examples)
readonly kSources=(libs apps examples)

# Xcode's copies on macOS; elsewhere those on PATH, which must come from the LLVM of the Clang that built it.
llvm_tool() {
    if command -v xcrun >/dev/null 2>&1; then
        xcrun "$@"
    else
        "$@"
    fi
}

# Runs a command with its output in build/coverage/<name>.log, and stops with the log's end if it fails.
quietly() {
    local name="$1"
    shift
    local log="$build/$name.log"
    if ! "$@" >"$log" 2>&1; then
        tail -n 40 "$log"
        echo
        echo "coverage.sh: $name failed (full output: ${log#"$repo"/})."
        exit 1
    fi
}

mkdir -p "$build"
quietly configure cmake --preset coverage
quietly build cmake --build --preset coverage
rm -rf "$profiles"
quietly test ctest --preset coverage

shopt -s nullglob
raw=("$profiles"/*.profraw)
shopt -u nullglob
if [[ ${#raw[@]} -eq 0 ]]; then
    echo "coverage.sh: the tests left no profiles in ${profiles#"$repo"/}."
    exit 1
fi
quietly merge llvm_tool llvm-profdata merge -sparse "${raw[@]}" -o "$profdata"

# Every program the build made: the test programs, which run, and the apps and examples, which do not, so that
# code only they contain counts as not reached rather than being left out.
objects=()
for candidate in "$build"/tests/*_tests "$build"/apps/*/*.app/Contents/MacOS/* "$build"/apps/*/visual_debug_* \
    "$build"/apps/document_lab/document_lab "$build"/apps/document_lab/media_smoke "$build"/examples/*/*; do
    if [[ -f "$candidate" && -x "$candidate" ]]; then
        objects+=("$candidate")
    fi
done
arguments=("${objects[0]}")
for ((i = 1; i < ${#objects[@]}; ++i)); do
    arguments+=(-object "${objects[i]}")
done
arguments+=(-instr-profile "$profdata")

# The TOTAL line of llvm-cov's report: lines covered and functions executed, as percentages. Its warning, the
# same for every report, is printed once at the end.
totals() {
    llvm_tool llvm-cov report "${arguments[@]}" "$@" 2>/dev/null |
        awk '$1 == "TOTAL" { print $10, $7 }'
}

# One line of the table, printed and kept in build/coverage/summary.txt, which CI shows on the run's page.
row() {
    local name="$1"
    shift
    local lines functions
    read -r lines functions < <(totals "$@")
    printf '%-22s %9s %11s\n' "$name" "${lines:-none}" "${functions:-none}" | tee -a "$build/summary.txt"
}

printf '%-22s %9s %11s\n' "" "lines" "functions" | tee "$build/summary.txt"
for area in "${kAreas[@]}"; do
    row "$area" "$repo/$area"
done
sources=()
for source in "${kSources[@]}"; do
    sources+=("$repo/$source")
done
row "all of them" "${sources[@]}"

llvm_tool llvm-cov report "${arguments[@]}" "${sources[@]}" >"$build/report.txt" 2>"$build/report-warnings.log"
rm -rf "$build/html"
quietly html llvm_tool llvm-cov show "${arguments[@]}" -format=html -output-dir="$build/html" "${sources[@]}"
echo "coverage.sh: file by file in ${build#"$repo"/}/report.txt, line by line in ${build#"$repo"/}/html/index.html."
# Inline functions that Qt's macros write into many files, such as tr() and metatype ids, and the tests'
# helpers, can carry different hashes in different files; llvm-cov leaves those out (the build guide says more).
if [[ -s "$build/report-warnings.log" ]]; then
    echo "coverage.sh: llvm-cov $(head -n 1 "$build/report-warnings.log"), left out: Qt's macro-made functions such as tr(), and test helpers."
fi
