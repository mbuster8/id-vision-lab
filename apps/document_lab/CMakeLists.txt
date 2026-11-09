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
