# runnable examples
# every examples/*.embr has a golden examples/<name>.out and is executed by CTest on each backend

set(PLUGIN_DIR "${BIN_DIR}/plugins")
add_plugin(hello hello.cpp)            # examples/hello/hello.cpp -> bin/<platform>/plugins/hello.so

if(NOT BUILD_TESTING)
    return()
endif()

set(example_work "${BIN_DIR}/test-work")
file(GLOB example_scripts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.embr")
foreach(script ${example_scripts})
    get_filename_component(ex "${script}" NAME_WE)
    # examples that need the VM (coroutines) say so on their first line: "# requires: vm"
    file(STRINGS "${script}" first_line LIMIT_COUNT 1)
    set(needs_vm FALSE)
    if(first_line MATCHES "requires: vm")
        set(needs_vm TRUE)
    endif()
    # "# requires: posix" means it uses something windows lacks (os_exec), so skip it there
    if(first_line MATCHES "requires: posix" AND WIN32)
        continue()
    endif()

    foreach(backend tw vm)
        if(backend STREQUAL "tw" AND (needs_vm OR NOT EMBR_WITH_TREE_WALKER))
            continue()
        endif()
        if(backend STREQUAL "vm" AND NOT EMBR_WITH_VM)
            continue()
        endif()
        set(vm_flag "")
        if(backend STREQUAL "vm")
            set(vm_flag "--vm")
        endif()
        set(test_name example_${ex}_${backend})
        file(MAKE_DIRECTORY "${example_work}/${test_name}")
        add_test(NAME ${test_name}
            COMMAND ${CMAKE_COMMAND}
                -DEMBR=$<TARGET_FILE:embr> -DVM_FLAG=${vm_flag}
                -DSCRIPT=${script} -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/${ex}.out
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_example.cmake
            WORKING_DIRECTORY "${example_work}/${test_name}")
        set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR}" TIMEOUT 60)
    endforeach()
endforeach()
