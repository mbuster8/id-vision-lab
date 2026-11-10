# The model files the Document Lab's processors load, resolved at configure time and never committed. Each is looked
# for, in order, in ID_VISION_LAB_MODEL_DIR (a folder of the owner's choosing, such as a copy for offline work), in
# the per-user cache that every build tree shares, and only then downloaded from its pinned release into that cache.
# Every copy found or fetched is checked against its SHA-256: a wrong copy in the cache is fetched again, a wrong
# copy in the owner's folder is an error. The build reads XDG_CACHE_HOME, HOME and LOCALAPPDATA itself; the
# programs read no environment variable for this.
# Also used in script mode by tests/cmake/model_lookup_test.cmake.

set(ID_VISION_LAB_MODEL_DIR "" CACHE PATH
    "A folder holding the model files, used before the per-user cache and the network")

# The per-user model cache: %LOCALAPPDATA%\id-vision-lab\models on Windows, else $XDG_CACHE_HOME/id-vision-lab/models
# or ~/.cache/id-vision-lab/models.
function(id_vision_lab_model_cache_dir out)
    if(CMAKE_HOST_WIN32 AND DEFINED ENV{LOCALAPPDATA})
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" base)
    elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{XDG_CACHE_HOME}" base)
    else()
        file(TO_CMAKE_PATH "$ENV{HOME}/.cache" base)
    endif()
    set(${out} "${base}/id-vision-lab/models" PARENT_SCOPE)
endfunction()

function(_id_vision_lab_matches file sha256 result)
    set(${result} FALSE PARENT_SCOPE)
    if(EXISTS "${file}" AND NOT IS_DIRECTORY "${file}")
        file(SHA256 "${file}" actual)
        string(TOLOWER "${sha256}" expected)
        if(actual STREQUAL expected)
            set(${result} TRUE PARENT_SCOPE)
        endif()
    endif()
endfunction()

# Copies build/models into a program after each build, where it looks for its models: the bundle's Resources/models
# on macOS, a models folder beside the program elsewhere (apps/document_lab/model_folder.cpp).
function(id_vision_lab_copy_models target)
    if(APPLE)
        set(destination "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/models")
    else()
        set(destination "$<TARGET_FILE_DIR:${target}>/models")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_BINARY_DIR}/models" "${destination}"
        VERBATIM)
endfunction()

# id_vision_lab_resolve_model(<out-var> FILE <name> URL <url> SHA256 <hex> [MODEL_DIR <dir>] [CACHE_DIR <dir>])
# Sets <out-var> to a copy of FILE whose SHA-256 is SHA256, or stops with an error saying where it looked.
# MODEL_DIR defaults to ID_VISION_LAB_MODEL_DIR, CACHE_DIR to id_vision_lab_model_cache_dir().
function(id_vision_lab_resolve_model out)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "FILE;URL;SHA256;MODEL_DIR;CACHE_DIR" "")
    if(NOT DEFINED ARG_MODEL_DIR)
        set(ARG_MODEL_DIR "${ID_VISION_LAB_MODEL_DIR}")
    endif()
    if(NOT DEFINED ARG_CACHE_DIR)
        id_vision_lab_model_cache_dir(ARG_CACHE_DIR)
    endif()
    set(where "the file ${ARG_FILE} (SHA-256 ${ARG_SHA256}, from ${ARG_URL})")

    if(NOT "${ARG_MODEL_DIR}" STREQUAL "")
        file(TO_CMAKE_PATH "${ARG_MODEL_DIR}" own)
        set(candidate "${own}/${ARG_FILE}")
        if(EXISTS "${candidate}")
            _id_vision_lab_matches("${candidate}" "${ARG_SHA256}" good)
            if(NOT good)
                message(FATAL_ERROR "${candidate} does not have the expected SHA-256 for ${where}. Replace it with "
                                    "the released file, or clear ID_VISION_LAB_MODEL_DIR.")
            endif()
            set(${out} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endif()

    set(cached "${ARG_CACHE_DIR}/${ARG_FILE}")
    _id_vision_lab_matches("${cached}" "${ARG_SHA256}" good)
    if(good)
        set(${out} "${cached}" PARENT_SCOPE)
        return()
    endif()

    file(MAKE_DIRECTORY "${ARG_CACHE_DIR}")
    set(part "${cached}.part")
    file(REMOVE "${part}")
    message(STATUS "Downloading ${ARG_FILE} into ${ARG_CACHE_DIR}")
    # The hash is checked here rather than with EXPECTED_HASH, which stops on a failed download before this can say
    # where it looked.
    file(DOWNLOAD "${ARG_URL}" "${part}" TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    set(reason "")
    if(NOT code EQUAL 0)
        list(GET status 1 reason)
    else()
        _id_vision_lab_matches("${part}" "${ARG_SHA256}" good)
        if(NOT good)
            set(reason "the downloaded file has another SHA-256")
        endif()
    endif()
    if(NOT reason STREQUAL "")
        file(REMOVE "${part}")
        message(FATAL_ERROR "Could not find or download ${where}: ${reason}. Looked in ID_VISION_LAB_MODEL_DIR "
                            "('${ARG_MODEL_DIR}') and the cache ${ARG_CACHE_DIR}. For an offline build, put the file "
                            "in a folder and set -DID_VISION_LAB_MODEL_DIR=<folder>.")
    endif()
    file(RENAME "${part}" "${cached}")
    set(${out} "${cached}" PARENT_SCOPE)
endfunction()
