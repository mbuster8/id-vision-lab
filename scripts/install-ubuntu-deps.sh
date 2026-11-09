#!/usr/bin/env bash
# The Ubuntu 24.04 packages that build and test the project: the distribution's OpenCV 4.6, Qt 6.4 and
# FFmpeg 6.1, the oldest versions it supports, and for the Python sender numpy, pyzmq, pytest and black (from
# apt: Ubuntu's Python is externally managed, so nothing is pip-installed). The CI's install-ubuntu-deps
# action and docker/ubuntu-24.04.Dockerfile both run this, so the list lives in one place. Needs root.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install --yes --no-install-recommends \
    build-essential cmake ninja-build pkgconf \
    qt6-base-dev qt6-multimedia-dev qt6-qpa-plugins libgl-dev \
    libopencv-dev libzmq3-dev cppzmq-dev nlohmann-json3-dev libgtest-dev \
    libavcodec-dev libavformat-dev libswscale-dev libavutil-dev \
    python3-numpy python3-zmq python3-pytest black \
    clang llvm libclang-rt-dev # the coverage report: Clang, its profile runtime, llvm-profdata and llvm-cov
