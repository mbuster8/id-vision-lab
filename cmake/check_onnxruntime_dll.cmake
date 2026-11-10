# cmake -DFOLDER=<a program's folder> -P <this>: fails the build when onnxruntime.dll is not beside the program, so
# Windows can never fall back to the older copy in System32 (cmake/runtime_dlls.cmake).
if(NOT EXISTS "${FOLDER}/onnxruntime.dll")
    message(FATAL_ERROR "onnxruntime.dll is not in ${FOLDER}: the program would load Windows' own, older copy. "
                        "Check that vcpkg built ONNX Runtime and that VCPKG_APPLOCAL_DEPS is on.")
endif()
