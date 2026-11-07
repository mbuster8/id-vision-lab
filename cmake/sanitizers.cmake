# AddressSanitizer and UndefinedBehaviorSanitizer, for the asan and windows-asan presets.

if(ID_VISION_LAB_SANITIZERS AND MSVC)
    # MSVC has AddressSanitizer only; macOS and Linux run UndefinedBehaviorSanitizer. Global rather than per
    # target, so every object in an executable uses the same instrumented runtime. Its runtime DLL is on PATH
    # in the Visual Studio developer environment, where the tests run.
    add_compile_options(/fsanitize=address)
    # ASan cannot work with the runtime checks CMake adds to Debug builds (/RTC1), nor with incremental links.
    foreach(flags CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
        string(REPLACE "/RTC1" "" ${flags} "${${flags}}")
    endforeach()
    add_link_options(/INCREMENTAL:NO)
    # vcpkg's OpenCV and GoogleTest, and Qt, are built without AddressSanitizer, and the standard library
    # annotates its strings and vectors only in instrumented code: one that crosses into those libraries would
    # be reported as a "container-overflow" that is not real. Every other check stays on.
    add_compile_definitions(_DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
elseif(ID_VISION_LAB_SANITIZERS)
    # Global rather than per target, so every object in an executable uses the same instrumented runtime.
    # No recovery: undefined behaviour aborts like an ASan error, so it fails the test instead of printing a
    # report into a passing run.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Compiles the sanitizer defaults into an executable, so they apply however it is started: by ctest, from
# Qt Creator, or by GoogleTest's test discovery during the build. MSVC turns the same check off through the
# definitions above instead.
function(id_vision_lab_add_sanitizer_defaults target)
    if(ID_VISION_LAB_SANITIZERS AND NOT MSVC)
        target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/cmake/sanitizer_defaults.cpp)
    endif()
endfunction()
