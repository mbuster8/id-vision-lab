# The Document Lab's vision side, free of Qt: the processors that run on each frame and, later, the engine that
# moves frames through them. live_sender uses its edges and its document outline too.
add_library(lab_core STATIC
    include/lab_core/document_outline.h
    include/lab_core/frame_processor.h
    include/lab_core/lab_result.h
    include/lab_core/processors.h
    src/document_outline.cpp
    src/processors.cpp)
add_library(lab_core::lab_core ALIAS lab_core)

target_include_directories(lab_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(lab_core PUBLIC cxx_std_20)

target_link_libraries(lab_core
    PUBLIC
        frame_io::frame_io
        visual_debug::visual_debug
        opencv_core
    PRIVATE
        opencv_imgproc)

id_vision_lab_set_warnings(lab_core)
