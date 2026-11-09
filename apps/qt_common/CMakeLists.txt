add_library(qt_common STATIC
    about.cpp
    about.h
    appearance.cpp
    appearance.h
    application_identity.cpp
    application_identity.h
    controls.cpp
    controls.h
    image_file.cpp
    image_file.h
    image_tags_conversion.cpp
    image_tags_conversion.h
    image_view.cpp
    image_view.h
    live_converter.cpp
    live_converter.h
    outline_icons.cpp
    outline_icons.h
    overlay_painter.cpp
    overlay_painter.h
    qimage_conversion.cpp
    qimage_conversion.h
    shades.cpp
    shades.h)

# The live converter runs a thread of its own.
find_package(Threads REQUIRED)

target_include_directories(qt_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# visual_debug is public: the tag conversion's header names its ImageDescription.
target_link_libraries(qt_common
    PUBLIC
        Qt6::Widgets
        opencv_core
        viewer_core::viewer_core
        visual_debug::visual_debug
        Threads::Threads
    PRIVATE
        opencv_imgcodecs
        opencv_imgproc)

id_vision_lab_set_warnings(qt_common)
