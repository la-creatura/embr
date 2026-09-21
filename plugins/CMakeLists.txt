set(PLUGIN_DIR "${BIN_DIR}/plugins")

function(add_plugin name source)

    add_library(${name} SHARED "${name}/${source}")

    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/include")
    #target_include_directories(${name} PRIVATE "${PLUGIN_DIR}/${name}/include")

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""
    )

    set_target_properties(${name} PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
        #ARCHIVE_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
    )
endfunction()

add_plugin(embrlib embrlib.cpp)
add_plugin(str str.cpp)
add_plugin(embrffi embrffi.cpp)
if(MINGW)

    target_include_directories(embrffi PRIVATE
        ${CMAKE_SOURCE_DIR}/include/libffi/include
    )

    #target_link_directories(embrffi PRIVATE
    #    ${CMAKE_SOURCE_DIR}/include/libffi/lib
    #)

    #target_link_libraries(embrffi PRIVATE ffi)
    target_link_libraries(embrffi PRIVATE
        ${CMAKE_SOURCE_DIR}/include/libffi/lib/libffi.a
    )

else()

    find_package(PkgConfig REQUIRED)

    pkg_check_modules(LIBFFI REQUIRED libffi)

    target_link_libraries(embrffi PRIVATE ${LIBFFI_LIBRARIES})

    target_include_directories(embrffi PRIVATE
        ${LIBFFI_INCLUDE_DIRS}
    )

endif()
