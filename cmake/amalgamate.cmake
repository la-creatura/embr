# amalgamate.cmake
#
# embr_amalgamate(<output_target_name>
#     ENTRY      <path to entry header, e.g. include/embr/core/root.h>
#     OUTPUT     <path to generated header, e.g. ${CMAKE_BINARY_DIR}/generated/embr/embr.h>
#     SOURCE_DIR <root of the modular headers to depend on, e.g. include/embr>
#     [INCLUDE_DIRS <dir> ...]        # extra -I search dirs, default: include/
#     [DEFINES <NAME[=VALUE]> ...]
#     [BANNER  <text>]
#     [PUBLIC_INCLUDE_DIR <dir>]      # dir added to consumers' include path;
#                                     # defaults to OUTPUT's own directory
# )
#
# custom command that regenerates OUTPUT whenever ENTRY or any file under SOURCE_DIR changes
# plus an INTERFACE target <name> that consumers can link against to get a build-order dependency on the
# generation step and PUBLIC_INCLUDE_DIR on their include path.
#
# PUBLIC_INCLUDE_DIR matters whenever consumer code does `#include
# <embr/embr.h>` rather than `#include <embr.h>`: put OUTPUT at
# .../generated/embr/embr.h and pass PUBLIC_INCLUDE_DIR .../generated (the
# *parent* of embr/) so the `embr/` path component in the #include still
# resolves. Left unset, it defaults to OUTPUT's own directory, which is
# correct only for a bare `#include <embr.h>`.
#
# usage:
#   embr_amalgamate(embr_single_header
#       ENTRY             ${CMAKE_SOURCE_DIR}/include/embr/core/root.h
#       OUTPUT            ${CMAKE_BINARY_DIR}/generated/embr/embr.h
#       SOURCE_DIR        ${CMAKE_SOURCE_DIR}/include/embr
#       INCLUDE_DIRS      ${CMAKE_SOURCE_DIR}/include
#       PUBLIC_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated
#       BANNER            "embr - single header build"
#   )
#   target_link_libraries(embr PRIVATE embr_single_header)

function(embr_amalgamate name)
    set(options "")
    set(oneValueArgs ENTRY OUTPUT SOURCE_DIR BANNER PUBLIC_INCLUDE_DIR)
    set(multiValueArgs INCLUDE_DIRS DEFINES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_ENTRY OR NOT ARG_OUTPUT OR NOT ARG_SOURCE_DIR)
        message(FATAL_ERROR "embr_amalgamate(${name}): ENTRY, OUTPUT and SOURCE_DIR are required")
    endif()

    if(NOT ARG_INCLUDE_DIRS)
        set(ARG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include")
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # dependency list: the script only ever inlines files reachable from
    # ENTRY, but tracking that exact set at configure time means re-running
    # CMake's own configure step on every header add/remove. Globbing every
    # header under SOURCE_DIR is a deliberate over-approximation: it costs
    # a few unnecessary reruns of the (cheap, pure-python) script on unrelated
    # header edits, in exchange for never silently going stale.
    file(GLOB_RECURSE ARG_DEPS CONFIGURE_DEPENDS
        "${ARG_SOURCE_DIR}/*.h" "${ARG_SOURCE_DIR}/*.hpp" "${ARG_SOURCE_DIR}/*.inl")

    set(SCRIPT "${CMAKE_SOURCE_DIR}/tools/amalgamate.py")

    set(CMD ${Python3_EXECUTABLE} "${SCRIPT}" "${ARG_ENTRY}" -o "${ARG_OUTPUT}")
    foreach(dir ${ARG_INCLUDE_DIRS})
        list(APPEND CMD -I "${dir}")
    endforeach()
    foreach(def ${ARG_DEFINES})
        list(APPEND CMD -D "${def}")
    endforeach()
    if(ARG_BANNER)
        list(APPEND CMD --banner "${ARG_BANNER}")
    endif()

    add_custom_command(
        OUTPUT  "${ARG_OUTPUT}"
        COMMAND ${CMD}
        DEPENDS "${SCRIPT}" "${ARG_ENTRY}" ${ARG_DEPS}
        COMMENT "amalgamating ${name} -> ${ARG_OUTPUT}"
        VERBATIM
    )

    add_custom_target(${name}_generate DEPENDS "${ARG_OUTPUT}")

    add_library(${name} INTERFACE)
    add_dependencies(${name} ${name}_generate)

    if(ARG_PUBLIC_INCLUDE_DIR)
        set(PUB_DIR "${ARG_PUBLIC_INCLUDE_DIR}")
    else()
        get_filename_component(PUB_DIR "${ARG_OUTPUT}" DIRECTORY)
    endif()
    target_include_directories(${name} INTERFACE "${PUB_DIR}")
endfunction()
