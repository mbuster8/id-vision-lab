# The Linux ONNX Runtime manifest (scripts/onnxruntime-vcpkg/vcpkg.json) pins the same vcpkg baseline as the
# repository's own vcpkg.json, so Windows and Linux build the same ONNX Runtime. Run as a script:
# cmake -DSOURCE_DIR=<repo> -P <this>.
cmake_minimum_required(VERSION 3.25)

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "Pass -DSOURCE_DIR=...")
endif()

set(root "${SOURCE_DIR}/vcpkg.json")
set(linux "${SOURCE_DIR}/scripts/onnxruntime-vcpkg/vcpkg.json")
foreach(manifest "${root}" "${linux}")
    if(NOT EXISTS "${manifest}")
        message(FATAL_ERROR "Missing ${manifest}")
    endif()
endforeach()

file(READ "${root}" root_json)
file(READ "${linux}" linux_json)
string(JSON root_baseline GET "${root_json}" builtin-baseline)
string(JSON linux_baseline GET "${linux_json}" builtin-baseline)
if(NOT root_baseline STREQUAL linux_baseline)
    message(FATAL_ERROR "vcpkg baselines differ: vcpkg.json ${root_baseline}, "
                        "scripts/onnxruntime-vcpkg/vcpkg.json ${linux_baseline}")
endif()
message(STATUS "vcpkg baseline ${root_baseline} in both manifests")
