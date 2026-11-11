# The model lookup of cmake/models.cmake, run as a script: cmake -DSOURCE_DIR=<repo> -DWORK_DIR=<scratch> -P <this>.
# A fake model is served from a file:// URL, so nothing reaches the network.
cmake_minimum_required(VERSION 3.25)

foreach(variable SOURCE_DIR WORK_DIR)
    if(NOT ${variable})
        message(FATAL_ERROR "Pass -D${variable}=...")
    endif()
endforeach()

include("${SOURCE_DIR}/cmake/models.cmake")

file(REMOVE_RECURSE "${WORK_DIR}")
set(server "${WORK_DIR}/server")
set(own "${WORK_DIR}/own")
set(cache "${WORK_DIR}/cache")
file(MAKE_DIRECTORY "${server}" "${own}" "${cache}")
file(WRITE "${server}/fake.onnx" "model")
file(SHA256 "${server}/fake.onnx" hash)
set(url "file://${server}/fake.onnx")

function(expect_equal actual expected what)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${what}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

# 1. The right file in the owner's folder is used from there.
file(COPY_FILE "${server}/fake.onnx" "${own}/fake.onnx")
id_vision_lab_resolve_model(found FILE fake.onnx URL "${url}" SHA256 ${hash} MODEL_DIR "${own}" CACHE_DIR "${cache}")
expect_equal("${found}" "${own}/fake.onnx" "the owner's folder")

# 2. A wrong file in the owner's folder is an error, never replaced.
file(WRITE "${own}/fake.onnx" "tampered")
set(driver "${WORK_DIR}/wrong_in_own.cmake")
file(WRITE "${driver}" "include(\"${SOURCE_DIR}/cmake/models.cmake\")\n"
    "id_vision_lab_resolve_model(found FILE fake.onnx URL \"${url}\" SHA256 ${hash} MODEL_DIR \"${own}\" "
    "CACHE_DIR \"${cache}\")\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${driver}" RESULT_VARIABLE result OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
if(result EQUAL 0 OR NOT out MATCHES "SHA-256")
    message(FATAL_ERROR "a wrong file in the owner's folder: expected an error naming the SHA-256, got ${result}: ${out}")
endif()
file(READ "${own}/fake.onnx" kept)
expect_equal("${kept}" "tampered" "the owner's file is left alone")

# 3. The right file in the cache is used without the network.
file(COPY_FILE "${server}/fake.onnx" "${cache}/fake.onnx")
file(RENAME "${server}/fake.onnx" "${server}/away.onnx")
id_vision_lab_resolve_model(found FILE fake.onnx URL "${url}" SHA256 ${hash} CACHE_DIR "${cache}")
expect_equal("${found}" "${cache}/fake.onnx" "the cache")
file(RENAME "${server}/away.onnx" "${server}/fake.onnx")

# 4. A wrong file in the cache is downloaded again.
file(WRITE "${cache}/fake.onnx" "stale")
id_vision_lab_resolve_model(found FILE fake.onnx URL "${url}" SHA256 ${hash} CACHE_DIR "${cache}")
expect_equal("${found}" "${cache}/fake.onnx" "the cache, replaced")
file(SHA256 "${cache}/fake.onnx" replaced)
expect_equal("${replaced}" "${hash}" "the replaced file's hash")

# 5. With no copy anywhere, the file is downloaded into the cache.
file(REMOVE "${cache}/fake.onnx")
id_vision_lab_resolve_model(found FILE fake.onnx URL "${url}" SHA256 ${hash} CACHE_DIR "${cache}")
expect_equal("${found}" "${cache}/fake.onnx" "downloaded into the cache")

# 6. Nowhere to be found or fetched: an error naming the file, the variable and the folder.
file(REMOVE "${cache}/fake.onnx")
set(driver "${WORK_DIR}/unreachable.cmake")
file(WRITE "${driver}" "include(\"${SOURCE_DIR}/cmake/models.cmake\")\n"
    "id_vision_lab_resolve_model(found FILE fake.onnx URL \"file://${server}/missing.onnx\" SHA256 ${hash} "
    "CACHE_DIR \"${cache}\")\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${driver}" RESULT_VARIABLE result OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
if(result EQUAL 0 OR NOT out MATCHES "fake.onnx" OR NOT out MATCHES "ID_VISION_LAB_MODEL_DIR")
    message(FATAL_ERROR "an unreachable model: expected an error naming it, got ${result}: ${out}")
endif()

message(STATUS "model lookup: all cases passed")
