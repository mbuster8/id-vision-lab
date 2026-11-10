#!/usr/bin/env bash
# Runs inside the id-vision-lab-ubuntu container (docker/ubuntu-24.04.Dockerfile): the same steps as the CI's
# Ubuntu job, on a copy of the source mounted read-only at /src. The copy leaves out build trees, .git and the
# Python sender's local environment, so the host's own builds are never touched or reused.
set -euo pipefail

# ONNX Runtime from the image (docker/ubuntu-24.04.Dockerfile), found through onnxruntime_ROOT alone.
export onnxruntime_ROOT="$(cat /opt/onnxruntime/root)"

mkdir -p /work
tar --exclude=./build --exclude=./.git --exclude=./python/.venv -C /src -cf - . | tar -C /work -xf -
cd /work

echo "== Ubuntu 24.04: the Python sender, format and tests"
python3 -c 'import sys, numpy, zmq, pytest; print("Python", sys.version.split()[0], "numpy", numpy.__version__, "pyzmq", zmq.__version__, "pytest", pytest.__version__)'
black --version
black --check python
python3 -m pytest python/tests -q

echo "== Ubuntu 24.04: configure, build and test (debug)"
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

echo "== Ubuntu 24.04: build and test without Qt"
cmake --preset debug -B build/no-qt -DID_VISION_LAB_BUILD_QT_APPS=OFF
cmake --build build/no-qt
ctest --test-dir build/no-qt --output-on-failure

echo "== Ubuntu 24.04: test coverage, built with Clang (a report, no threshold)"
CXX=clang++ scripts/coverage.sh
