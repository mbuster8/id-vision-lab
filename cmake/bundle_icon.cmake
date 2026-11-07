# Gives an app its icon, made at build time from one square PNG (1024 pixels is best), so the repository holds
# only the PNG: on macOS the bundle's Finder and Dock icon (.icns), on Windows the .exe's icon in Explorer, the
# taskbar and Alt-Tab (.ico, compiled in through a generated .rc). Elsewhere it does nothing: the windows get
# their icon from Qt at run time.
function(id_vision_lab_add_bundle_icon target png)
    if(APPLE)
        set(script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/make_icns.cmake")
        set(icns "${CMAKE_CURRENT_BINARY_DIR}/${target}.icns")
        add_custom_command(OUTPUT "${icns}"
            COMMAND "${CMAKE_COMMAND}" "-DPNG=${png}" "-DICNS=${icns}" -P "${script}"
            DEPENDS "${png}" "${script}"
            COMMENT "Making ${target}.icns"
            VERBATIM)
        target_sources(${target} PRIVATE "${icns}")
        set_source_files_properties("${icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE "${target}.icns")
    elseif(WIN32)
        # The tool that writes the .ico, built once for every app that calls this.
        if(NOT TARGET id_vision_lab_make_ico)
            find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
            add_executable(id_vision_lab_make_ico "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/make_ico.cpp")
            target_link_libraries(id_vision_lab_make_ico PRIVATE opencv_core opencv_imgproc opencv_imgcodecs)
            # No Qt in it: apps/ turns automatic moc, uic and rcc on for every target.
            set_target_properties(id_vision_lab_make_ico PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
            id_vision_lab_set_warnings(id_vision_lab_make_ico)
        endif()
        set(ico "${CMAKE_CURRENT_BINARY_DIR}/${target}.ico")
        set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
        add_custom_command(OUTPUT "${ico}"
            COMMAND id_vision_lab_make_ico "${png}" "${ico}"
            DEPENDS "${png}" id_vision_lab_make_ico
            COMMENT "Making ${target}.ico"
            VERBATIM)
        # rc finds the .ico next to the .rc; the first icon in the file is the one Windows shows.
        file(WRITE "${rc}" "IDI_ICON1 ICON \"${target}.ico\"\n")
        set_source_files_properties("${rc}" PROPERTIES OBJECT_DEPENDS "${ico}")
        target_sources(${target} PRIVATE "${ico}" "${rc}")
    endif()
endfunction()
