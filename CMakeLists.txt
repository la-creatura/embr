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
    BANNER             "embr - single header build (tree-walker backend)"
)
# PUBLIC_INCLUDE_DIR is generated/ (not generated/embr/) so that the existing #include <embr/embr.h> keeps resolving

add_executable(embr src/cli.cpp)
target_link_libraries(embr PRIVATE embr_single_header)

add_executable(embr_tests src/tests.cpp)
target_link_libraries(embr_tests PRIVATE embr_single_header)

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

set_target_properties(embr       PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}")
set_target_properties(embr_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}")

add_subdirectory(plugins)
