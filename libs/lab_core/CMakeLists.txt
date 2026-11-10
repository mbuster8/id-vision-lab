# The Document Lab's vision side, free of Qt: the processors that run on each frame (the Document processor on
# doc_vision), the sender that takes their results to the viewer, and the engine that moves frames through them.
# live_sender uses its edges and its document outline too.
find_package(Threads REQUIRED)

add_library(lab_core STATIC
    include/lab_core/document_outline.h
    include/lab_core/document_processor.h
    include/lab_core/engine.h
    include/lab_core/frame_processor.h
    include/lab_core/lab_result.h
    include/lab_core/processors.h
    include/lab_core/result_sender.h
    include/lab_core/viewer_link.h
    src/document_outline.cpp
    src/document_processor.cpp
    src/engine.cpp
    src/number_text.h
    src/processors.cpp
    src/result_sender.cpp
    src/viewer_link.cpp)
add_library(lab_core::lab_core ALIAS lab_core)

target_include_directories(lab_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(lab_core PUBLIC cxx_std_20)

target_link_libraries(lab_core
    PUBLIC
        doc_vision::doc_vision
        frame_io::frame_io
        visual_debug::visual_debug
        opencv_core
        Threads::Threads
    PRIVATE
        opencv_imgproc)

id_vision_lab_set_warnings(lab_core)
