# One GoogleTest executable per area. Each test case runs in its own process, and the timeout turns a hung
# socket into a failure instead of a stall. Each program also runs once whole, as IDEs such as Qt Creator run
# it: state one test leaves behind breaks the next only when they share a process. Discovery runs the new
# binary once at build time; antivirus scanning can make that first launch slow, hence the longer timeout.
# Callers pass GTest::gtest_main, or GTest::gtest with their own main().
function(id_vision_lab_add_test_executable target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LIBRARIES")
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES})
    id_vision_lab_set_warnings(${target})
    id_vision_lab_add_sanitizer_defaults(${target})
    gtest_discover_tests(${target} DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 60)
    add_test(NAME "${target} (whole program)" COMMAND ${target})
    set_tests_properties("${target} (whole program)" PROPERTIES LABELS whole TIMEOUT 120)
endfunction()

id_vision_lab_add_test_executable(visual_debug_tests
    SOURCES
        visual_debug/wire_format_test.cpp
        visual_debug/overlay_wire_test.cpp
        visual_debug/endpoint_test.cpp
        visual_debug/client_server_test.cpp
        visual_debug/send_image_test.cpp
        visual_debug/image_sender_test.cpp
        visual_debug/header_cases_test.cpp
    LIBRARIES
        visual_debug::visual_debug
        cppzmq
        nlohmann_json::nlohmann_json
        GTest::gtest_main)
# The golden headers the C++ and the Python encoders must both write (python/tests/test_golden_headers.py).
target_compile_definitions(visual_debug_tests
    PRIVATE ID_VISION_LAB_HEADER_CASES="${PROJECT_SOURCE_DIR}/tests/wire/header_cases.json")

id_vision_lab_add_test_executable(viewer_core_tests
    SOURCES
        viewer_core/display_converter_test.cpp
        viewer_core/display_settings_test.cpp
        viewer_core/hdr_tone_map_test.cpp
        viewer_core/histogram_test.cpp
        viewer_core/region_statistics_test.cpp
        viewer_core/image_history_test.cpp
        viewer_core/image_tags_test.cpp
        viewer_core/live_channels_test.cpp
        viewer_core/pixel_inspector_test.cpp
        viewer_core/save_name_test.cpp
        viewer_core/viewport_test.cpp
    LIBRARIES
        viewer_core::viewer_core
        opencv_imgproc
        GTest::gtest_main)

# frame_io's readers, which write and read clips of their own; on Windows with the UTF-8 code page, as the
# programs that use them run. FFmpeg's libraries are optional: the tests of its reader, and the clips they write
# through it, only when CMake found them (the target is GLOBAL, made in libs/frame_io).
set(frame_io_ffmpeg_tests "")
set(frame_io_ffmpeg_libraries "")
if(TARGET PkgConfig::FFMPEG)
    set(frame_io_ffmpeg_tests frame_io/ffmpeg_clip.cpp frame_io/ffmpeg_clip.h frame_io/ffmpeg_video_test.cpp)
    set(frame_io_ffmpeg_libraries PkgConfig::FFMPEG)
endif()
id_vision_lab_add_test_executable(frame_io_tests
    SOURCES
        frame_io/capture_test.cpp
        frame_io/frame_test.cpp
        frame_io/image_folder_test.cpp
        frame_io/read_ahead_test.cpp
        frame_io/recording_measurement_test.cpp
        frame_io/recording_test.cpp
        frame_io/replay_pacer_test.cpp
        frame_io/scratch_folder.h
        frame_io/video_file_test.cpp
        frame_io/video_source_test.cpp
        frame_io/yuv_test.cpp
        ${frame_io_ffmpeg_tests}
    LIBRARIES
        frame_io::frame_io
        ${frame_io_ffmpeg_libraries}
        opencv_videoio
        opencv_imgcodecs
        opencv_imgproc
        nlohmann_json::nlohmann_json
        cppzmq
        GTest::gtest_main)
id_vision_lab_use_utf8_code_page(frame_io_tests)
# latch.h: holding a reader's thread without timing (Qt-free, kept with the other test helpers);
# test_support.h: the measurements' environment variables.
target_include_directories(frame_io_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/qt_common
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/visual_debug)

id_vision_lab_add_test_executable(lab_core_tests
    SOURCES
        lab_core/card_frame.h
        lab_core/document_outline_test.cpp
        lab_core/engine_test.cpp
        lab_core/fake_sender.h
        lab_core/list_source.h
        lab_core/processors_test.cpp
        lab_core/result_sender_test.cpp
    LIBRARIES
        lab_core::lab_core
        opencv_imgproc
        nlohmann_json::nlohmann_json
        GTest::gtest_main)
# latch.h: holding a sending thread without timing; scratch_folder.h: a recording's folder.
target_include_directories(lab_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/qt_common
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/frame_io)

if(ID_VISION_LAB_BUILD_EXAMPLES)
    id_vision_lab_add_test_executable(pipeline_tests
        SOURCES
            examples/pipeline_test.cpp
        LIBRARIES
            pipeline
            visual_debug::visual_debug
            GTest::gtest_main)
    # The examples' file reading, tested as the examples run: with the UTF-8 code page on Windows.
    id_vision_lab_use_utf8_code_page(pipeline_tests)
    id_vision_lab_add_test_executable(live_sender_tests
        SOURCES
            examples/live_sender_test.cpp
            examples/real_time_frames_test.cpp
        LIBRARIES
            live_sender_lib
            lab_core::lab_core
            frame_io::frame_io
            opencv_videoio
            opencv_imgproc
            opencv_imgcodecs
            visual_debug::visual_debug
            cppzmq
            nlohmann_json::nlohmann_json
            GTest::gtest_main)
    # test_support.h: the collecting sink the delivery test uses; card_frame.h: the outline's synthetic card.
    target_include_directories(live_sender_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/visual_debug
                                                         ${CMAKE_CURRENT_SOURCE_DIR}/lab_core)
    id_vision_lab_use_utf8_code_page(live_sender_tests)
endif()

# The Qt code, with an offscreen QApplication. Every Qt test program uses qt_common/test_main.cpp, which keeps
# the tests' settings apart from the owner's. The apps get a program each: both have a main_window.h.
if(ID_VISION_LAB_BUILD_QT_APPS)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
    if(ID_VISION_LAB_COVERAGE)
        # Coverage matches a function's counts by its name and the shape of its branches. This main() would
        # leave counts under "main" that the apps' and examples' own main(), which no test runs, could match
        # or clash with; without them, each of those counts as not reached.
        set_source_files_properties(qt_common/test_main.cpp
            PROPERTIES COMPILE_OPTIONS "-fno-profile-instr-generate;-fno-coverage-mapping")
    endif()
    id_vision_lab_add_test_executable(qt_common_tests
        SOURCES
            qt_common/test_main.cpp
            qt_common/about_test.cpp
            qt_common/appearance_test.cpp
            qt_common/image_file_test.cpp
            qt_common/image_tags_conversion_test.cpp
            qt_common/image_view_test.cpp
            qt_common/live_converter_test.cpp
            qt_common/live_drawing_test.cpp
            qt_common/outline_icons_test.cpp
            qt_common/overlay_painter_test.cpp
            qt_common/owner_only.cpp
            qt_common/owner_only_test.cpp
            qt_common/qimage_conversion_test.cpp
            qt_common/shades_test.cpp
        LIBRARIES
            qt_common
            viewer_core::viewer_core
            visual_debug::visual_debug
            opencv_imgcodecs
            opencv_imgproc
            Qt6::Test
            GTest::gtest)
    id_vision_lab_add_test_executable(viewer_ui_tests
        SOURCES
            qt_common/test_main.cpp
            viewer/activity_log_panel_test.cpp
            viewer/activity_log_test.cpp
            viewer/data_folder_test.cpp
            viewer/display_settings_control_test.cpp
            viewer/histogram_strip_test.cpp
            viewer/image_info_bar_test.cpp
            viewer/image_sidebar_test.cpp
            viewer/image_store_test.cpp
            viewer/live_channel_panel_test.cpp
            viewer/main_window_test.cpp
            qt_common/owner_only.cpp
            viewer/qt_image_sink_test.cpp
            viewer/server_controls_test.cpp
            viewer/snapshot_converter_test.cpp
            viewer/storage_worker_test.cpp
            viewer/thumbnail_panel_test.cpp
        LIBRARIES
            viewer_ui
            opencv_imgcodecs
            Qt6::Test
            GTest::gtest)
    id_vision_lab_add_test_executable(example_sender_ui_tests
        SOURCES
            qt_common/test_main.cpp
            example_sender/main_window_test.cpp
            example_sender/send_counter_test.cpp
        LIBRARIES
            example_sender_ui
            opencv_imgcodecs
            Qt6::Test
            GTest::gtest)
    # The Document Lab's: Qt Multimedia's frames, and the window with fake cameras. With the UTF-8 code page
    # and, on Windows, vcpkg's FFmpeg beside it, as the lab runs.
    id_vision_lab_add_test_executable(document_lab_ui_tests
        SOURCES
            qt_common/test_main.cpp
            qt_common/owner_only.cpp
            document_lab/camera_formats_test.cpp
            document_lab/camera_frame_test.cpp
            document_lab/fake_camera_backend.h
            document_lab/main_window_test.cpp
            document_lab/measurement_test.cpp
        LIBRARIES
            document_lab_ui
            opencv_imgproc
            cppzmq
            nlohmann_json::nlohmann_json
            Qt6::Test
            GTest::gtest)
    id_vision_lab_use_utf8_code_page(document_lab_ui_tests)
    id_vision_lab_copy_ffmpeg_dlls(document_lab_ui_tests)
    # The shared Qt test helpers, such as palette_support.h and control_checks.h.
    target_include_directories(viewer_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/qt_common)
    target_include_directories(example_sender_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/qt_common)
    # The lab's also share the engine's fakes (fake_sender.h, list_source.h, card_frame.h) and scratch_folder.h.
    target_include_directories(document_lab_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/qt_common
                                                             ${CMAKE_CURRENT_SOURCE_DIR}/lab_core
                                                             ${CMAKE_CURRENT_SOURCE_DIR}/frame_io
                                                             ${CMAKE_CURRENT_SOURCE_DIR}/visual_debug)
    # Qt Multimedia's backend beside frame_io's readers (apps/document_lab/media_smoke.cpp), a program of its
    # own so that it also runs from the installed Windows package.
    add_test(NAME media_smoke COMMAND media_smoke)
    set_tests_properties(media_smoke PROPERTIES TIMEOUT 120)
endif()
