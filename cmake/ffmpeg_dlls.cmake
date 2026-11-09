# On Windows with vcpkg, copies vcpkg's FFmpeg DLLs (avcodec, avformat, avutil, swresample, swscale) next to a
# target after each build. Qt Multimedia's FFmpeg plug-in imports swresample, which nothing of the project's
# imports, so vcpkg's own copying leaves it out; the plug-in would then load Qt's copy from QT_ROOT_DIR\bin on
# PATH beside vcpkg's other four: two FFmpeg builds in one process. With the whole set next to the executable,
# which Windows searches first, every FFmpeg DLL a lab process loads is vcpkg's. Elsewhere it does nothing:
# macOS and Linux load one FFmpeg by its own library names.
function(id_vision_lab_copy_ffmpeg_dlls target)
    if(NOT WIN32 OR NOT DEFINED VCPKG_INSTALLED_DIR)
        return()
    endif()
    set(bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
    endif()
    file(GLOB dlls "${bin}/avcodec-*.dll" "${bin}/avformat-*.dll" "${bin}/avutil-*.dll" "${bin}/swresample-*.dll"
         "${bin}/swscale-*.dll")
    list(LENGTH dlls count)
    if(NOT count EQUAL 5)
        message(FATAL_ERROR "Expected vcpkg's five FFmpeg DLLs in ${bin}, found: ${dlls}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${dlls} "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()
