cmake_minimum_required(VERSION 3.20)

project(embr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# unit tests run through CTest (see tests/CMakeLists.txt) instead of one
# hand-rolled embr_tests binary -- each plugin gets its own small, separately
# buildable/runnable test executable
include(CTest)

# which execution backend(s) end up compiled into embr.h -- and therefore
# into every target that links embr_single_header (embr itself, every
# plugin, every test_* binary) -- is chosen here, not in the source tree.
# see include/embr/backends/tree_walker.h and backends/vm.h: both are always
# #include'd by core/root.h, but each one's *content* is a no-op unless its
# own EMBR_WITH_* macro is defined, which happens via the DEFINES passed to
# embr_amalgamate() below (baked in as #defines at the top of the generated
# header). at least one must be on.
option(EMBR_WITH_TREE_WALKER "Compile in the AST tree-walking backend" ON)
option(EMBR_WITH_VM          "Compile in the bytecode VM backend"      OFF)

if(NOT EMBR_WITH_TREE_WALKER AND NOT EMBR_WITH_VM)
    message(FATAL_ERROR "at least one of EMBR_WITH_TREE_WALKER / EMBR_WITH_VM must be ON")
endif()

set(EMBR_BACKEND_DEFINES "")
set(EMBR_BACKEND_NAMES   "")
if(EMBR_WITH_TREE_WALKER)
    list(APPEND EMBR_BACKEND_DEFINES EMBR_WITH_TREE_WALKER)
    list(APPEND EMBR_BACKEND_NAMES   "tree-walker")
endif()
if(EMBR_WITH_VM)
    list(APPEND EMBR_BACKEND_DEFINES EMBR_WITH_VM)
    list(APPEND EMBR_BACKEND_NAMES   "vm")
endif()
list(JOIN EMBR_BACKEND_NAMES " + " EMBR_BACKEND_NAMES_STR)

# embr.h is generated at build time from the modular sources under include/embr/{core,backends}/
# edit the modular headers
# every target below depends on the regenerated header and rebuilds it automatically.
include(cmake/amalgamate.cmake)

embr_amalgamate(embr_single_header
    ENTRY              ${CMAKE_SOURCE_DIR}/include/embr/core/root.h
    OUTPUT             ${CMAKE_BINARY_DIR}/generated/embr/embr.h
    SOURCE_DIR         ${CMAKE_SOURCE_DIR}/include/embr
    INCLUDE_DIRS       ${CMAKE_SOURCE_DIR}/include
    PUBLIC_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated
    DEFINES            ${EMBR_BACKEND_DEFINES}
    BANNER             "embr - single header build (backends: ${EMBR_BACKEND_NAMES_STR})"
)
# PUBLIC_INCLUDE_DIR is generated/ (not generated/embr/) so that the existing #include <embr/embr.h> keeps resolving

add_executable(embr src/cli.cpp)
target_link_libraries(embr PRIVATE embr_single_header)

target_compile_options(embr PRIVATE
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-g>
)

if(WIN32)
    set(BIN_DIR ${CMAKE_SOURCE_DIR}/bin/windows)
    foreach(dll
        libstdc++-6.dll
        libgcc_s_seh-1.dll
        libwinpthread-1.dll
    )
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${dll}
            OUTPUT_VARIABLE DLL_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        add_custom_command(TARGET embr POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_PATH}"
                "$<TARGET_FILE_DIR:embr>"
        )
    endforeach()
else()
    set(BIN_DIR ${CMAKE_SOURCE_DIR}/bin/linux)
endif()

set_target_properties(embr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}")

add_subdirectory(plugins)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
