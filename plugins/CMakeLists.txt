set(PLUGIN_DIR "${BIN_DIR}/plugins")

function(add_plugin name source)

    # cmake target names must be unique across the whole build, but a plugin's
    # source lives under plugins/${name}/ and its output .so/.dll should be
    # named ${name} regardless -- so the cmake target gets a "_plugin" suffix
    # to avoid colliding with same-named things linked into it (e.g. system
    # libffi's own "ffi" library name), while OUTPUT_NAME keeps the plugin's
    # real name.
    set(target "${name}_plugin")

    add_library(${target} SHARED "${name}/${source}")

    # plugins consume the generated single header, same as embr and the
    # test_* binaries under tests/, so they rebuild automatically whenever
    # a modular header changes
    target_link_libraries(${target} PRIVATE embr_single_header)

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
    )
endfunction()

add_plugin(embrlib embrlib.cpp)
add_plugin(str str.cpp)
add_plugin(json json.cpp)
add_plugin(ffi ffi.cpp)

# mingw needs packaged libffi because pkg-config pulls in glibc headers that don't exist in windows environment
if(MINGW)

    target_include_directories(ffi_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/include/libffi/include
    )

    target_link_libraries(ffi_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/include/libffi/lib/libffi.a
    )

# linux can use system libffi via pkg-config.
else()

    find_package(PkgConfig REQUIRED)

    pkg_check_modules(LIBFFI REQUIRED libffi)

    target_link_libraries(ffi_plugin PRIVATE ${LIBFFI_LIBRARIES})

    target_include_directories(ffi_plugin PRIVATE
        ${LIBFFI_INCLUDE_DIRS}
    )

endif()
