# Everything but the reading of cameras and files as a library, so the unit tests can check it without a
# camera. Only the executable and live_sender_video need OpenCV's videoio module. read_ahead.h is header-only;
# it and run()'s edges use threads.
find_package(Threads REQUIRED)
add_library(live_sender_lib STATIC
    live_sender.cpp
    live_sender.h
    read_ahead.h)
target_include_directories(live_sender_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(live_sender_lib PUBLIC opencv_core visual_debug::visual_debug Threads::Threads
                                      PRIVATE opencv_imgproc)
id_vision_lab_set_warnings(live_sender_lib)

find_package(OpenCV REQUIRED COMPONENTS videoio)

# Opening video files: which backend tries first, testable with clips the tests write.
add_library(live_sender_video STATIC
    video_file.cpp
    video_file.h)
target_include_directories(live_sender_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(live_sender_video PUBLIC opencv_videoio)
id_vision_lab_set_warnings(live_sender_video)

add_executable(live_sender main.cpp)
target_link_libraries(live_sender PRIVATE live_sender_lib live_sender_video opencv_videoio)
id_vision_lab_set_warnings(live_sender)
id_vision_lab_add_sanitizer_defaults(live_sender)
id_vision_lab_use_utf8_code_page(live_sender)

# --hdr reads a video with FFmpeg's own libraries: OpenCV's videoio gives 8-bit pictures without the 10-bit
# data and the HDR tags. Optional: without them live_sender still builds, and --hdr says what is missing.
# GLOBAL, so tests/ can link the clips it writes against the same target.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET GLOBAL libavformat libavcodec libswscale libavutil)
endif()
if(FFMPEG_FOUND)
    add_library(live_sender_ffmpeg STATIC
        ffmpeg_video.cpp
        ffmpeg_video.h)
    target_include_directories(live_sender_ffmpeg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(live_sender_ffmpeg PUBLIC opencv_core visual_debug::visual_debug
                                             PRIVATE PkgConfig::FFMPEG)
    id_vision_lab_set_warnings(live_sender_ffmpeg)
    target_link_libraries(live_sender PRIVATE live_sender_ffmpeg)
    target_compile_definitions(live_sender PRIVATE LIVE_SENDER_HAS_FFMPEG=1)
else()
    message(STATUS "FFmpeg's libraries (libavformat, libavcodec, libswscale, libavutil) not found: "
                   "live_sender builds without --hdr")
endif()
