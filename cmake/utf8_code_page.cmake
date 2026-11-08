# On Windows, makes UTF-8 a program's code page through its manifest, so its argv and the narrow paths that
# cv::imread, cv::imwrite and cv::VideoCapture open files by are UTF-8, as they are on macOS and Linux: a file
# name such as "obrázok č.1.png" then works. For the examples, which use plain main() and OpenCV's file API;
# the Qt apps go through Qt's wide-character file API instead. Elsewhere it does nothing.
function(id_vision_lab_use_utf8_code_page target)
    if(WIN32)
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8_code_page.manifest")
    endif()
endfunction()
