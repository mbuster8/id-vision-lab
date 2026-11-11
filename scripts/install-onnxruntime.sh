#!/usr/bin/env bash
# Builds ONNX Runtime with vcpkg, at the baseline of scripts/onnxruntime-vcpkg/vcpkg.json (the repository's own), for
# Linux: the Ubuntu CI job and docker/ubuntu-24.04.Dockerfile. Shared libraries, release only (the triplets beside
# the manifest), so CMake finds it through onnxruntime_ROOT alone and Ubuntu's other packages stay what the build
# uses. Never Microsoft's prebuilt packages (ADR-004). The vcpkg clone is removed afterwards; only
# <prefix>/installed/<triplet> is kept.
#
#     scripts/install-onnxruntime.sh <prefix>
#
# Prints onnxruntime_ROOT=<prefix>/installed/<triplet> as its last line. Needs git, curl, zip, unzip, tar, cmake,
# ninja, pkg-config and a C++ compiler. ONNXRUNTIME_MAX_JOBS caps the parallel jobs (default 6: the build runs out of
# memory with too many).
set -euo pipefail

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
}

if [[ $# -ne 1 || "$1" == "--help" || "$1" == "-h" ]]; then
    usage
    [[ $# -eq 1 ]] && exit 0
    exit 2
fi

prefix="$1"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
manifest="$here/onnxruntime-vcpkg"
baseline="$(sed -n 's/.*"builtin-baseline": *"\([0-9a-f]*\)".*/\1/p' "$manifest/vcpkg.json")"
if [[ -z "$baseline" ]]; then
    echo "No builtin-baseline in $manifest/vcpkg.json" >&2
    exit 1
fi

case "$(uname -m)" in
x86_64) arch=x64 ;;
aarch64 | arm64)
    arch=arm64
    # vcpkg has no tools of its own for arm64 Linux: it uses the system's cmake, ninja and pkg-config.
    export VCPKG_FORCE_SYSTEM_BINARIES=1
    ;;
*)
    echo "Unsupported machine: $(uname -m)" >&2
    exit 1
    ;;
esac
triplet="$arch-linux-dynamic-release"

jobs="$(nproc)"
cap="${ONNXRUNTIME_MAX_JOBS:-6}"
if ((jobs > cap)); then
    jobs="$cap"
fi
export VCPKG_MAX_CONCURRENCY="$jobs"

mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
rm -rf "$prefix/vcpkg"
# Full history: vcpkg reads the ports' versions at the baseline from git.
git clone --quiet https://github.com/microsoft/vcpkg "$prefix/vcpkg"
git -C "$prefix/vcpkg" checkout --quiet "$baseline"
"$prefix/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
"$prefix/vcpkg/vcpkg" install \
    --x-manifest-root="$manifest" \
    --overlay-triplets="$manifest/triplets" \
    --triplet="$triplet" \
    --x-install-root="$prefix/installed" \
    --clean-after-build
rm -rf "$prefix/vcpkg"
echo "onnxruntime_ROOT=$prefix/installed/$triplet"
