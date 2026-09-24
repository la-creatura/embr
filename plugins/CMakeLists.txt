set(PLUGIN_DIR "${BIN_DIR}/plugins")

function(add_plugin name source)

    add_library(${name} SHARED "${name}/${source}")

    # plugins consume the generated single header, same as embr/embr_tests,
    # so they rebuild automatically whenever a modular header changes
    target_link_libraries(${name} PRIVATE embr_single_header)

    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""
    )

    set_target_properties(${name} PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
    )
endfunction()

add_plugin(embrlib embrlib.cpp)
add_plugin(str str.cpp)
add_plugin(embrffi embrffi.cpp)

# mingw needs packaged libffi because pkg-config pulls in glibc headers that don't exist in windows environment
if(MINGW)

    target_include_directories(embrffi PRIVATE
        ${CMAKE_SOURCE_DIR}/include/libffi/include
    )

    target_link_libraries(embrffi PRIVATE
        ${CMAKE_SOURCE_DIR}/include/libffi/lib/libffi.a
    )

# linux can use system libffi via pkg-config.
else()

    find_package(PkgConfig REQUIRED)

    pkg_check_modules(LIBFFI REQUIRED libffi)

    target_link_libraries(embrffi PRIVATE ${LIBFFI_LIBRARIES})

    target_include_directories(embrffi PRIVATE
        ${LIBFFI_INCLUDE_DIRS}
    )

endif()
