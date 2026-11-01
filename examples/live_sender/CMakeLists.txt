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
