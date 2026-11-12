cmake_minimum_required(VERSION 3.25)

project(IdVisionLab
    VERSION 2.0.0
    DESCRIPTION "ID Vision Lab: a visual debugger for OpenCV images, growing into a lab for reading identity documents"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ID_VISION_LAB_BUILD_QT_APPS "Build the Qt 6 viewer and example sender" ON)
option(ID_VISION_LAB_BUILD_EXAMPLES "Build the command-line examples" ON)
option(ID_VISION_LAB_BUILD_TESTS "Build the GoogleTest unit tests" ON)
option(ID_VISION_LAB_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ID_VISION_LAB_COVERAGE "Build with Clang's source-based code coverage" OFF)

include(cmake/compiler_warnings.cmake)
include(cmake/sanitizers.cmake)
include(cmake/coverage.cmake)
include(cmake/utf8_code_page.cmake)
include(cmake/ffmpeg_dlls.cmake)
include(cmake/models.cmake)
include(cmake/runtime_dlls.cmake)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(cppzmq REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
# The CNN runtime of the document vision: Homebrew's on macOS, vcpkg's on Windows and Linux (ADR-004), never
# Microsoft's own builds.
find_package(onnxruntime 1.31 REQUIRED)

# DocAligner LC100, the page detector (Apache-2.0, DocsaidLab's release 1.1.1), into build/models for the programs
# and the tests.
id_vision_lab_resolve_model(ID_VISION_LAB_LC100_MODEL
    FILE lcnet100_h_e_bifpn_256_fp32.onnx
    URL https://github.com/DocsaidLab/DocAligner/releases/download/1.1.1/lcnet100_h_e_bifpn_256_fp32.onnx
    SHA256 f4117b786e3a18470f3865c93f3c2bd69d9b998edd60f385574a5c665e79594e)
file(COPY "${ID_VISION_LAB_LC100_MODEL}" DESTINATION "${CMAKE_BINARY_DIR}/models")
# MRZScanner's two stages, the MRZ reader (Apache-2.0, DocsaidLab's release 1.0.7): the band's detection, then its
# recognition (ADR-004 decision 7).
id_vision_lab_resolve_model(ID_VISION_LAB_MRZ_DETECTION_MODEL
    FILE mrz_detection_20250222_fp32.onnx
    URL https://github.com/DocsaidLab/MRZScanner/releases/download/1.0.7/mrz_detection_20250222_fp32.onnx
    SHA256 0786844728d828dc68c19b844aa87e2b29fdba3a587468f2863824d116ffc269)
id_vision_lab_resolve_model(ID_VISION_LAB_MRZ_RECOGNITION_MODEL
    FILE mrz_recognition_20250221_fp32.onnx
    URL https://github.com/DocsaidLab/MRZScanner/releases/download/1.0.7/mrz_recognition_20250221_fp32.onnx
    SHA256 07cef987fc1852aac12f18e1269fbaf1e2644579ca5743061d6f9cec7bef77c0)
file(COPY "${ID_VISION_LAB_MRZ_DETECTION_MODEL}" "${ID_VISION_LAB_MRZ_RECOGNITION_MODEL}"
     DESTINATION "${CMAKE_BINARY_DIR}/models")

add_subdirectory(libs/visual_debug)
add_subdirectory(libs/viewer_core)
add_subdirectory(libs/frame_io)
add_subdirectory(libs/doc_vision)
add_subdirectory(libs/lab_core)

if(ID_VISION_LAB_BUILD_QT_APPS)
    add_subdirectory(apps)
endif()

if(ID_VISION_LAB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ID_VISION_LAB_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_subdirectory(tests)
endif()
