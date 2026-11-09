# The Document Lab. Qt Multimedia is found here, not in apps/CMakeLists.txt, so a Qt without it still builds the
# viewer and the example sender.
find_package(Qt6 6.4 REQUIRED COMPONENTS Multimedia)

# A check that Qt Multimedia's backend loads beside frame_io's video readers, run by ctest and, on Windows, from
# the installed package (see cmake/windows_packaging.cmake).
add_executable(media_smoke media_smoke.cpp)
target_link_libraries(media_smoke PRIVATE Qt6::Multimedia frame_io::frame_io opencv_videoio)
id_vision_lab_set_warnings(media_smoke)
id_vision_lab_add_sanitizer_defaults(media_smoke)
id_vision_lab_use_utf8_code_page(media_smoke)
id_vision_lab_copy_ffmpeg_dlls(media_smoke)

# The lab's window and its parts, a library so that the tests can use them.
add_library(document_lab_ui STATIC
    camera_frame.cpp
    camera_frame.h)
target_include_directories(document_lab_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(document_lab_ui
    PUBLIC
        Qt6::Widgets
        Qt6::Multimedia
        qt_common
        frame_io::frame_io
        lab_core::lab_core
    PRIVATE
        opencv_imgcodecs
        opencv_imgproc)
id_vision_lab_set_warnings(document_lab_ui)
