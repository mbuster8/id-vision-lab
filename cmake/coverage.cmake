# Clang's source-based code coverage, for the coverage preset. scripts/coverage.sh builds it, runs the tests
# and reports how much of the libraries, the apps and the examples they reach.

if(ID_VISION_LAB_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ID_VISION_LAB_COVERAGE needs Clang or Apple Clang; ${CMAKE_CXX_COMPILER_ID} has no "
                            "source-based coverage")
    endif()
    # Global, so every library, app, example and test is instrumented; the report leaves the tests themselves,
    # generated code and other projects' headers out.
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()
