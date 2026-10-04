cmake_minimum_required(VERSION 3.20)

project(embr VERSION 1.0.0 LANGUAGES CXX)

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

# the docs live in the GitHub wiki, which is the git submodule in wiki/ (git submodule update --init). CTest uses it
# to check the docs. to use another clone instead:  cmake --preset linux-vm-debug -DEMBR_WIKI_DIR=../some-clone
# (the env var of the same name works too). if neither exists the wiki checks are skipped
if(NOT EMBR_WIKI_DIR AND DEFINED ENV{EMBR_WIKI_DIR})
    set(EMBR_WIKI_DIR "$ENV{EMBR_WIKI_DIR}")
endif()
if(NOT EMBR_WIKI_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/wiki/home.md")
    set(EMBR_WIKI_DIR "${CMAKE_SOURCE_DIR}/wiki")
endif()
set(EMBR_WIKI_DIR "${EMBR_WIKI_DIR}" CACHE PATH "the wiki clone the docs checks in CTest read (default: the wiki/ submodule)")
if(EMBR_WIKI_DIR AND NOT IS_DIRECTORY "${EMBR_WIKI_DIR}")
    message(WARNING "EMBR_WIKI_DIR=${EMBR_WIKI_DIR} is not a folder, the wiki docs checks will be skipped")
    set(EMBR_WIKI_DIR "")
endif()

# unit tests run through CTest
include(CTest)

# which execution backend(s) end up compiled into embr.h
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

# embr.h is generated at build time
# every target below depends on the regenerated header and rebuilds it automatically
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
elseif(APPLE)
    set(BIN_DIR ${CMAKE_SOURCE_DIR}/bin/macos)
else()
    set(BIN_DIR ${CMAKE_SOURCE_DIR}/bin/linux)
endif()

set_target_properties(embr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}")

# which build directory produced this binary
get_filename_component(EMBR_BUILD_NAME "${CMAKE_BINARY_DIR}" NAME)
target_compile_definitions(embr PRIVATE EMBR_BUILD_NAME="${EMBR_BUILD_NAME}" EMBR_VERSION="${PROJECT_VERSION}")

# a stamp next to the binary saying which build directory wrote it last
add_custom_command(TARGET embr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DNAME=${EMBR_BUILD_NAME} -DOUT=${BIN_DIR}/.built-by -P ${CMAKE_SOURCE_DIR}/cmake/write_stamp.cmake
    VERBATIM
)

add_subdirectory(plugins)
add_subdirectory(modules)
add_subdirectory(examples)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# cmake --install <build dir> --prefix <dir>
# gives prefix/bin/embr, prefix/lib/embr/{plugins,modules}, prefix/include/embr/embr.h and prefix/share/doc/embr
# the plugins and modules are installed by their own CMakeLists
# the embr binary finds prefix/lib/embr by itself
install(TARGETS embr RUNTIME DESTINATION bin)
install(FILES "${CMAKE_BINARY_DIR}/generated/embr/embr.h" DESTINATION include/embr)
install(FILES README.md LICENSE DESTINATION share/doc/embr)

# cmake --build <build dir> --target package     makes embr-<version>-<os>-<arch>.tar.gz
string(TOLOWER "${CMAKE_SYSTEM_NAME}" EMBR_PACKAGE_OS)
set(CPACK_PACKAGE_NAME embr)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
# a toolchain file that sets CMAKE_SYSTEM_NAME leaves CMAKE_SYSTEM_PROCESSOR empty, so fall back to the host's
set(EMBR_PACKAGE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(NOT EMBR_PACKAGE_ARCH)
    set(EMBR_PACKAGE_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${EMBR_PACKAGE_ARCH}" EMBR_PACKAGE_ARCH)
set(CPACK_PACKAGE_FILE_NAME "embr-${PROJECT_VERSION}-${EMBR_PACKAGE_OS}-${EMBR_PACKAGE_ARCH}")
set(CPACK_GENERATOR TGZ)
if(WIN32)
    set(CPACK_GENERATOR ZIP)
endif()
include(CPack)
