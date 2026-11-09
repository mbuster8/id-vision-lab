# Reading and recording frames, for the Document Lab and live_sender: video files through OpenCV's videoio, and
# with FFmpeg's libraries for 10-bit HDR as 16-bit data with its tags. No Qt, so the no-Qt build and A3's tests
# can use it.
find_package(Threads REQUIRED)

add_library(frame_io STATIC
    src/video_file.cpp)
add_library(frame_io::frame_io ALIAS frame_io)

target_include_directories(frame_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(frame_io PUBLIC cxx_std_20)

target_link_libraries(frame_io
    PUBLIC
        opencv_core
        opencv_videoio
        visual_debug::visual_debug
        Threads::Threads
    PRIVATE
        opencv_imgcodecs
        opencv_imgproc
        nlohmann_json::nlohmann_json)

id_vision_lab_set_warnings(frame_io)

# FFmpeg's libraries are optional: without them frame_io reads 8-bit video only, and live_sender's --hdr says
# what is missing. GLOBAL, so the tests can write their clips against the same target.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET GLOBAL libavformat libavcodec libswscale libavutil)
endif()
if(FFMPEG_FOUND)
    target_sources(frame_io PRIVATE src/ffmpeg_video.cpp)
    target_link_libraries(frame_io PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(frame_io PUBLIC FRAME_IO_HAS_FFMPEG=1)
else()
    message(STATUS "FFmpeg's libraries (libavformat, libavcodec, libswscale, libavutil) not found: "
                   "frame_io reads 8-bit video only, and live_sender builds without --hdr")
endif()
