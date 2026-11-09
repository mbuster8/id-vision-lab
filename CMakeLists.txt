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

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(cppzmq REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

add_subdirectory(libs/visual_debug)
add_subdirectory(libs/viewer_core)
add_subdirectory(libs/frame_io)

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
