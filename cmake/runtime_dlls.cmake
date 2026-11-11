# On Windows, puts the release build of ONNX Runtime, and the release DLLs it needs, beside a program after each
# build, then checks that onnxruntime.dll is there (cmake/copy_onnxruntime.cmake). Elsewhere it does nothing:
# macOS and Linux find their libraries through the RPATH.
#
# Why beside: Windows 11 ships an older onnxruntime.dll in System32 for Windows ML, which a program without a copy
# beside it would load. Why release, in debug builds too: vcpkg's debug onnxruntime.dll fails ONNX's debug-only
# check of its operator schemas when it loads a model ("SchemasRegisterer: Assertion ... failed"). Programs use
# only ONNX Runtime's C API, through its header-only C++ wrapper, so a debug program and the release DLL share no
# C++ objects or heap across the boundary. Only ONNX Runtime and the DLLs it alone uses are copied: Qt's DLLs
# beside a program would make Qt look for its plug-ins in the program's folder.
#
# One at a time: the test programs share one folder, and each one's link step ends with vcpkg's applocal copy
# (the debug DLLs), GoogleTest's discovery (which runs the program, loading them) and this copy (the release
# ones). Run side by side, one target's copy met another's open re2.dll (seen on a 32-thread build), so these
# links share a Ninja pool of one.
if(WIN32)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS id_vision_lab_onnxruntime_dlls=1)
endif()

function(id_vision_lab_copy_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()
    get_target_property(release onnxruntime::onnxruntime IMPORTED_LOCATION_RELEASE)
    if(NOT release)
        get_target_property(release onnxruntime::onnxruntime IMPORTED_LOCATION)
    endif()
    if(NOT release)
        message(FATAL_ERROR "onnxruntime::onnxruntime names no release DLL: cannot put it beside ${target}.")
    endif()
    get_filename_component(release_folder "${release}" DIRECTORY)
    set_property(TARGET ${target} PROPERTY JOB_POOL_LINK id_vision_lab_onnxruntime_dlls)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DFROM=${release_folder}" "-DFOLDER=$<TARGET_FILE_DIR:${target}>"
                -P "${PROJECT_SOURCE_DIR}/cmake/copy_onnxruntime.cmake"
        VERBATIM)
endfunction()
