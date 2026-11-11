# The Document Lab. Qt Multimedia is found here, not in apps/CMakeLists.txt, so a Qt without it still builds the
# viewer and the example sender, without the lab and its tests.
find_package(Qt6 6.4 QUIET OPTIONAL_COMPONENTS Multimedia)
if(NOT TARGET Qt6::Multimedia)
    message(STATUS "Qt Multimedia not found: the Document Lab is not built")
    return()
endif()

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
    camera_backend.cpp
    camera_backend.h
    camera_formats.cpp
    camera_formats.h
    camera_frame.cpp
    camera_frame.h
    lab_settings.cpp
    lab_settings.h
    main_window.cpp
    main_window.h
    model_folder.cpp
    model_folder.h
    scan_banner.cpp
    scan_banner.h
    scan_style.cpp
    scan_style.h
    scan_words.cpp
    scan_words.h)
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

# The app. Resources belong to the executable: a static library's are dropped by the linker unless initialised
# by hand. qt_add_executable's finaliser reads the Info.plist and links the camera permission plug-in that its
# NSCameraUsageDescription needs; it is also imported by name, so a Qt whose finaliser does not still has it.
qt_add_executable(document_lab MACOSX_BUNDLE
    main.cpp
    resources.qrc)
set_target_properties(document_lab PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Document Lab"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.proximasus.document-lab"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    WIN32_EXECUTABLE ON)
target_compile_definitions(document_lab PRIVATE ID_VISION_LAB_VERSION="${PROJECT_VERSION}")
target_link_libraries(document_lab PRIVATE document_lab_ui)
if(APPLE AND TARGET Qt6::QDarwinCameraPermissionPlugin)
    qt_import_plugins(document_lab INCLUDE Qt6::QDarwinCameraPermissionPlugin)
endif()
id_vision_lab_add_bundle_icon(document_lab "${PROJECT_SOURCE_DIR}/resources/icons/document_lab_icon.png")
id_vision_lab_use_utf8_code_page(document_lab)
id_vision_lab_copy_ffmpeg_dlls(document_lab)
id_vision_lab_copy_runtime_dlls(document_lab)
id_vision_lab_copy_models(document_lab)
id_vision_lab_set_warnings(document_lab)
id_vision_lab_add_sanitizer_defaults(document_lab)

# The camera focus spike (not part of the lab): which focus controls each camera offers through Qt, and what they
# do to a page's sharpness. A console program on Windows, so that its report shows in the terminal.
qt_add_executable(camera_focus_probe MACOSX_BUNDLE camera_focus_probe.cpp uvc_controls.cpp uvc_controls.h)
set_target_properties(camera_focus_probe PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Camera Focus Probe"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.proximasus.camera-focus-probe"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/camera_focus_probe.plist.in")
target_link_libraries(camera_focus_probe PRIVATE document_lab_ui opencv_imgproc)
if(APPLE)
    target_link_libraries(camera_focus_probe PRIVATE "-framework IOKit" "-framework CoreFoundation")
endif()
if(WIN32)
    target_link_libraries(camera_focus_probe PRIVATE strmiids ole32 oleaut32)
endif()
if(APPLE AND TARGET Qt6::QDarwinCameraPermissionPlugin)
    qt_import_plugins(camera_focus_probe INCLUDE Qt6::QDarwinCameraPermissionPlugin)
endif()
id_vision_lab_use_utf8_code_page(camera_focus_probe)
id_vision_lab_copy_ffmpeg_dlls(camera_focus_probe)
id_vision_lab_copy_runtime_dlls(camera_focus_probe)
id_vision_lab_copy_models(camera_focus_probe)
id_vision_lab_set_warnings(camera_focus_probe)
