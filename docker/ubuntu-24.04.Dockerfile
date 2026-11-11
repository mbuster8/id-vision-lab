# The CI's Ubuntu job on this machine: Ubuntu 24.04 with the distribution's OpenCV 4.6 and Qt 6.4. The packages
# are installed once, into the image; every run copies the source mounted at /src and builds it from scratch.
# scripts/check.sh builds and runs it; by hand, from the repository root:
#
#     docker build --tag id-vision-lab-ubuntu --file docker/ubuntu-24.04.Dockerfile scripts
#     docker run --rm --volume "$PWD":/src:ro id-vision-lab-ubuntu
#
# The build context is scripts/, not the repository, so the host's build trees are never sent to Docker.
FROM ubuntu:24.04

COPY install-ubuntu-deps.sh /tmp/install-ubuntu-deps.sh
RUN bash /tmp/install-ubuntu-deps.sh && rm -rf /var/lib/apt/lists/* /tmp/install-ubuntu-deps.sh

# ONNX Runtime, built once into the image from vcpkg at the repository's baseline (Ubuntu has no package of it at
# the version the project needs). One RUN, so the vcpkg clone the script removes never stays in a layer. The
# installed folder's path is kept in /opt/onnxruntime/root for run-ubuntu-tests.sh.
COPY install-onnxruntime.sh /tmp/ort/install-onnxruntime.sh
COPY onnxruntime-vcpkg /tmp/ort/onnxruntime-vcpkg
RUN bash /tmp/ort/install-onnxruntime.sh /opt/onnxruntime > /tmp/ort/log.txt 2>&1 \
    || (tail -n 80 /tmp/ort/log.txt; exit 1); \
    tail -n 1 /tmp/ort/log.txt | sed -n 's/^onnxruntime_ROOT=//p' > /opt/onnxruntime/root \
    && test -s /opt/onnxruntime/root && rm -rf /tmp/ort

# As on GitHub's runners: the C locale with UTF-8, and Qt without a display.
ENV LANG=C.UTF-8 QT_QPA_PLATFORM=offscreen

# Read from the mounted source, so a change to it needs no new image.
ENTRYPOINT ["bash", "/src/docker/run-ubuntu-tests.sh"]
