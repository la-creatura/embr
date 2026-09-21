cmake_minimum_required(VERSION 3.20)

project(embr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#target_compile_features(embr PRIVATE cxx_std_20)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(embr src/cli.cpp)
target_include_directories(embr PRIVATE include)

add_executable(embr_tests src/tests.cpp)
target_include_directories(embr_tests PRIVATE include)

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
