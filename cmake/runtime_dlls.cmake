# On Windows, copies the DLLs a target links (ONNX Runtime's among them) beside it after each build, then checks
# that onnxruntime.dll is there. Windows 11 ships an older onnxruntime.dll in System32 for Windows ML; a program
# without vcpkg's copy beside it would load that one. vcpkg's applocal step (VCPKG_APPLOCAL_DEPS) also copies the
# DLLs those DLLs import. Elsewhere it does nothing: macOS and Linux find their libraries through the RPATH.
function(id_vision_lab_copy_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_RUNTIME_DLLS:${target}>"
                "$<TARGET_FILE_DIR:${target}>"
        COMMAND "${CMAKE_COMMAND}" "-DFOLDER=$<TARGET_FILE_DIR:${target}>"
                -P "${PROJECT_SOURCE_DIR}/cmake/check_onnxruntime_dll.cmake"
        COMMAND_EXPAND_LISTS
        VERBATIM)
endfunction()
