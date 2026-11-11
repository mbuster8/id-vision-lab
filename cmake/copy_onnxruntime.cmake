# cmake -DFROM=<vcpkg's release bin folder> -DFOLDER=<a program's folder> -P <this> (cmake/runtime_dlls.cmake):
# copies the release onnxruntime.dll and the DLLs it imports that only it uses (vcpkg builds them as DLLs: Protocol
# Buffers, Abseil, RE2) beside the program, replacing the debug copies vcpkg's applocal step put there, and fails
# the build when onnxruntime.dll is not there afterwards, so Windows can never fall back to the older copy in
# System32.
foreach(name IN ITEMS onnxruntime.dll onnxruntime_providers_shared.dll libprotobuf.dll libprotobuf-lite.dll
                      abseil_dll.dll re2.dll)
    if(EXISTS "${FROM}/${name}")
        file(COPY_FILE "${FROM}/${name}" "${FOLDER}/${name}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
if(NOT EXISTS "${FOLDER}/onnxruntime.dll")
    message(FATAL_ERROR "onnxruntime.dll is not in ${FOLDER}: the program would load Windows' own, older copy. "
                        "Check that vcpkg built ONNX Runtime (${FROM}).")
endif()
