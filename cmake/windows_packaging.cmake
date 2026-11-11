# The Windows packages of the three apps, made by `cpack` from a windows-release build: a portable .zip and an
# .msi (WiX Toolset 3). Both hold the same folder: bin\ with the apps, Qt (deployed by windeployqt), the vcpkg
# DLLs they load and the Visual C++ runtime, and licenses\ with every licence that applies. Included from
# apps/CMakeLists.txt, where Qt's deployment functions are defined; elsewhere it does nothing.
if(NOT WIN32)
    return()
endif()
# 6.7 for windeployqt's --no-ffmpeg and --exclude-plugins, which keep Qt's own FFmpeg out of the package.
if(Qt6_VERSION VERSION_LESS 6.7)
    message(STATUS "Windows packages need Qt 6.7 or newer for windeployqt's --no-ffmpeg; not set up")
    return()
endif()
# A debug build loads the debug C++ runtime, which InstallRequiredSystemLibraries does not ship and Windows
# does not have without Visual Studio, so its packages would not start elsewhere.
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    message(STATUS "Windows packages are made from a release build (the windows-release preset); not set up")
    return()
endif()
if(NOT TARGET document_lab)
    message(STATUS "Windows packages hold the Document Lab, which needs Qt Multimedia; not set up")
    return()
endif()

include(GNUInstallDirs)
set(id_vision_lab_packaged_apps visual_debug_viewer visual_debug_example_sender document_lab)

# The apps, and the vcpkg DLLs they load, found where vcpkg installed them. Qt's DLLs come from windeployqt
# below, and Windows' own and the C++ runtime's are left to the system and to InstallRequiredSystemLibraries.
set(id_vision_lab_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
install(TARGETS ${id_vision_lab_packaged_apps}
    RUNTIME_DEPENDENCY_SET id_vision_lab_runtime
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(RUNTIME_DEPENDENCY_SET id_vision_lab_runtime
    PRE_EXCLUDE_REGEXES
        "^[Aa][Pp][Ii]-[Mm][Ss]-" "^[Ee][Xx][Tt]-[Mm][Ss]-" "^[Qq][Tt]6"
        "^[Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee]" "^[Mm][Ss][Vv][Cc][Pp]"
    POST_EXCLUDE_REGEXES "[Ss][Yy][Ss][Tt][Ee][Mm]32"
    DIRECTORIES "${id_vision_lab_vcpkg_bin}"
    DESTINATION ${CMAKE_INSTALL_BINDIR})

# One FFmpeg in the package, vcpkg's: all five of its DLLs, swresample too, which only Qt Multimedia's FFmpeg
# plug-in imports, so the plug-in loads these rather than a copy of Qt's (windeployqt's --no-ffmpeg below).
file(GLOB id_vision_lab_ffmpeg_dlls "${id_vision_lab_vcpkg_bin}/avcodec-*.dll" "${id_vision_lab_vcpkg_bin}/avformat-*.dll"
     "${id_vision_lab_vcpkg_bin}/avutil-*.dll" "${id_vision_lab_vcpkg_bin}/swresample-*.dll"
     "${id_vision_lab_vcpkg_bin}/swscale-*.dll")
list(LENGTH id_vision_lab_ffmpeg_dlls id_vision_lab_ffmpeg_dll_count)
if(NOT id_vision_lab_ffmpeg_dll_count EQUAL 5)
    message(FATAL_ERROR "Expected vcpkg's five FFmpeg DLLs in ${id_vision_lab_vcpkg_bin}, found: "
                        "${id_vision_lab_ffmpeg_dlls}")
endif()
install(FILES ${id_vision_lab_ffmpeg_dlls} DESTINATION ${CMAKE_INSTALL_BINDIR})

# The check that Qt Multimedia's backend loads beside frame_io's readers, run from an installed package by CI;
# a component of its own, so neither package holds it.
install(TARGETS media_smoke RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT smoke EXCLUDE_FROM_ALL)

# Qt's DLLs and plug-ins (platform, styles, image formats), next to the apps. The compiler runtime is left to
# InstallRequiredSystemLibraries, which installs its DLLs, where windeployqt would add Microsoft's installer.
# Left out, about 40 MB: the apps paint with Qt's raster engine and use no OpenGL, Direct3D shaders or
# network (the touch-input plug-in alone would pull in Qt Network and its TLS plug-ins), and their text is
# English only, so Qt's translations would go unused.
foreach(app IN LISTS id_vision_lab_packaged_apps)
    set(deploy_options --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler
        --skip-plugin-types generic,networkinformation,tls)
    # The lab's multimedia: not Qt's FFmpeg DLLs (vcpkg's are installed above), and not the Windows Media
    # Foundation backend, which Qt has deprecated, so only the FFmpeg one loads.
    if(app STREQUAL "document_lab")
        list(APPEND deploy_options --no-ffmpeg --exclude-plugins windowsmediaplugin)
    endif()
    qt_generate_deploy_app_script(TARGET ${app} OUTPUT_SCRIPT deploy_script NO_COMPILER_RUNTIME NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR
        DEPLOY_TOOL_OPTIONS ${deploy_options})
    install(SCRIPT "${deploy_script}")
endforeach()

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
include(InstallRequiredSystemLibraries)

# Licences: the project's, a notice naming every third-party component, the GNU licences Qt is used under,
# and each shipped vcpkg port's own copyright file.
configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/NOTICE" "${CMAKE_BINARY_DIR}/NOTICE.txt" COPYONLY)
install(FILES
    "${CMAKE_BINARY_DIR}/LICENSE.txt"
    "${CMAKE_BINARY_DIR}/NOTICE.txt"
    "${PROJECT_SOURCE_DIR}/packaging/licenses/THIRD-PARTY-NOTICES.txt"
    "${PROJECT_SOURCE_DIR}/packaging/licenses/LGPL-3.0.txt"
    "${PROJECT_SOURCE_DIR}/packaging/licenses/GPL-3.0.txt"
    "${PROJECT_SOURCE_DIR}/packaging/licenses/Apache-2.0.txt"
    "${PROJECT_SOURCE_DIR}/packaging/licenses/DocAligner-NOTICE.txt"
    DESTINATION licenses)
foreach(port IN ITEMS opencv4 ffmpeg zeromq cppzmq nlohmann-json libjpeg-turbo libpng libwebp tiff zlib liblzma)
    install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${port}/copyright"
        DESTINATION licenses/vcpkg/${port})
endforeach()
# ONNX Runtime and the ports vcpkg builds it from (the list a Windows build made); a port this baseline does not
# build is skipped.
foreach(port IN ITEMS onnxruntime protobuf abseil onnx re2 flatbuffers eigen3 utf8-range date cpuinfo wil dlpack
                      optional-lite ms-gsl safeint boost-config boost-mp11)
    set(copyright "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${port}/copyright")
    if(EXISTS "${copyright}")
        install(FILES "${copyright}" DESTINATION licenses/vcpkg/${port})
    endif()
endforeach()
# The page model, next to the programs as the build tree has it (apps/document_lab/model_folder.cpp looks there).
install(DIRECTORY "${CMAKE_BINARY_DIR}/models/" DESTINATION ${CMAKE_INSTALL_BINDIR}/models)

set(CPACK_PACKAGE_NAME "ID Vision Lab")
set(CPACK_PACKAGE_VENDOR "Matej Suster")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "The Visual Debug Viewer, the Visual Debug Example Sender and the Document Lab")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/mbuster8/id-vision-lab")
set(CPACK_PACKAGE_FILE_NAME "id-vision-lab-${PROJECT_VERSION}-windows-x64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ID Vision Lab")
set(CPACK_PACKAGE_EXECUTABLES
    visual_debug_viewer "Visual Debug Viewer"
    visual_debug_example_sender "Visual Debug Example Sender"
    document_lab "Document Lab")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")
set(CPACK_GENERATOR "ZIP;WIX")
# Fixed for good: Windows recognises a newer .msi with the same upgrade code as an upgrade of the installed one.
set(CPACK_WIX_UPGRADE_GUID "409C4717-59C3-4D00-8210-5C88123BFDC1")
# For everyone on the computer, under Program Files, which asks for administrator rights; the .zip needs none.
set(CPACK_WIX_INSTALL_SCOPE "perMachine")
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_BINARY_DIR}/apps/viewer/visual_debug_viewer.ico")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "ID Vision Lab")
include(CPack)
