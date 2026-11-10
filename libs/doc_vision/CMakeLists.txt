# The Document Lab's document vision, free of Qt: ONNX Runtime and the page model, the page's geometry, the quality
# gates and the hints. ONNX Runtime's headers stay inside this library's sources, all compiled with
# ORT_API_MANUAL_INIT: the API is taken only after checking that the loaded library offers it (an older
# onnxruntime.dll ships with Windows), and MSVC's detect_mismatch requires every unit that includes them to agree.
add_library(doc_vision STATIC
    include/doc_vision/onnx_model.h
    src/onnx_model.cpp)
add_library(doc_vision::doc_vision ALIAS doc_vision)

target_include_directories(doc_vision PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(doc_vision PUBLIC cxx_std_20)
target_compile_definitions(doc_vision PRIVATE ORT_API_MANUAL_INIT)

target_link_libraries(doc_vision
    PUBLIC
        opencv_core
    PRIVATE
        onnxruntime::onnxruntime
        opencv_imgproc)

id_vision_lab_set_warnings(doc_vision)
