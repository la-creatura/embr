# each test suite is a small executable from a single .cpp and harness.h
# editing one plugin's tests only recompiles and relinks that one binary
# and so CMake/CTest can build and run them in parallel

function(add_embr_test name)
    add_executable(test_${name} ${name}_test.cpp)
    target_link_libraries(test_${name} PRIVATE embr_single_header)
    set_target_properties(test_${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/tests"
    )
    add_test(NAME ${name} COMMAND test_${name} WORKING_DIRECTORY "${BIN_DIR}")
endfunction()

add_embr_test(core)  # language + embrlib (embr's always-available "batteries" stdlib)
add_embr_test(str)
add_embr_test(json)
add_embr_test(ffi)

add_dependencies(test_core embrlib_plugin)
add_dependencies(test_str  str_plugin)
add_dependencies(test_json json_plugin)
add_dependencies(test_ffi  ffi_plugin)
