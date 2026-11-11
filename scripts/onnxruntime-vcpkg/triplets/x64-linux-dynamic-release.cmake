# ONNX Runtime and what it depends on, as shared libraries, release only: the project finds it through
# onnxruntime_ROOT alone and loads it through the build tree's RPATH, so nothing of vcpkg's shadows Ubuntu's own
# packages for the rest of the build.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
