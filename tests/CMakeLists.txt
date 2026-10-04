# each test suite is a small executable from a single .cpp and harness.h
# editing one plugin's tests only recompiles and relinks that one binary
# and so CMake/CTest can build and run them in parallel

# if another preset overwrote the shared bin/, this says so once, clearly, before other tests fail
# in a confusing way (see "the shared bin/ folder" in README.md)
add_test(NAME bin_dir_stamp COMMAND ${CMAKE_COMMAND}
    -DSTAMP=${BIN_DIR}/.built-by -DEXPECTED=${EMBR_BUILD_NAME} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_bin_stamp.cmake)

function(add_embr_test name)
    add_executable(test_${name} ${name}_test.cpp)
    target_link_libraries(test_${name} PRIVATE embr_single_header)
    set_target_properties(test_${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/tests"
    )
    # every registered test gets its OWN working directory. suites write fixture files (modules,
    # manifests, scratch dirs) into the cwd, and on a dual-backend build the _tw and _vm
    # registrations of one suite run concurrently under `ctest -j` -- a shared cwd let one process
    # truncate a fixture another was loading (async_vm flaked that way). the test binaries live in
    # bin/<platform>/tests/, so plugins/ and modules/ (one level up) are reached via EMBR_PATH.
    set(work_root "${BIN_DIR}/test-work")
    if(EMBR_WITH_TREE_WALKER AND EMBR_WITH_VM)
        # harness.h's runSourceAny() picks the backend from EMBR_TEST_BACKEND
        file(MAKE_DIRECTORY "${work_root}/${name}_tw" "${work_root}/${name}_vm")
        add_test(NAME ${name}_tw COMMAND test_${name} WORKING_DIRECTORY "${work_root}/${name}_tw")
        add_test(NAME ${name}_vm COMMAND test_${name} WORKING_DIRECTORY "${work_root}/${name}_vm")
        set_tests_properties(${name}_tw PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR};EMBR_TEST_BACKEND=tree")
        set_tests_properties(${name}_vm PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR};EMBR_TEST_BACKEND=vm")
    else()
        file(MAKE_DIRECTORY "${work_root}/${name}")
        add_test(NAME ${name} COMMAND test_${name} WORKING_DIRECTORY "${work_root}/${name}")
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR}")
    endif()
endfunction()

add_embr_test(core)  # language + embrlib (the "batteries" stdlib)
add_embr_test(str)
add_embr_test(unicode)
add_embr_test(json)
add_embr_test(embrtypes)
add_embr_test(ffi)
add_embr_test(embrmath)
add_embr_test(fs)
add_embr_test(path)
add_embr_test(os)
add_embr_test(random)
add_embr_test(encoding)
add_embr_test(csv)
add_embr_test(module_path)
add_embr_test(time)
add_embr_test(regex)
add_embr_test(embrmeta)
add_embr_test(ffi_embr)
add_embr_test(table)
add_embr_test(vec)
add_embr_test(parallel)
add_embr_test(parallel_embr)
add_embr_test(async)
add_embr_test(dunder)
add_embr_test(http)
add_embr_test(pkg_embr)
add_embr_test(gc)
add_embr_test(coroutine)
add_embr_test(vm_internals)

# a regression here (VmRunner::inject() mis-splicing on a second call, see
# vm_internals_test.cpp's own header) manifests as an infinite loop, not a
# clean failure -- a short TIMEOUT turns that into a fast, legible CTest
# failure instead of hanging the whole suite/CI.
if(EMBR_WITH_TREE_WALKER AND EMBR_WITH_VM)
    set_tests_properties(vm_internals_tw vm_internals_vm PROPERTIES TIMEOUT 10)
else()
    set_tests_properties(vm_internals PROPERTIES TIMEOUT 10)
endif()

add_dependencies(test_core       embrlib_plugin)
add_dependencies(test_str        str_plugin)
add_dependencies(test_unicode    unicode_plugin)
add_dependencies(test_json       json_plugin embrlib_plugin)
add_dependencies(test_embrtypes  embrtypes_plugin ffi_plugin)
add_dependencies(test_ffi        ffi_plugin embrtypes_plugin embrlib_plugin)
add_dependencies(test_embrmath   embrmath_plugin)
add_dependencies(test_fs         fs_plugin embrlib_plugin)
add_dependencies(test_path       path_plugin)
add_dependencies(test_os         os_plugin embrlib_plugin)
add_dependencies(test_random     random_plugin embrlib_plugin)
add_dependencies(test_encoding   encoding_plugin embrlib_plugin)
add_dependencies(test_csv        csv_plugin embrlib_plugin)
add_dependencies(test_module_path os_plugin fs_plugin path_plugin embrlib_plugin)
add_dependencies(test_time       embrlib_plugin str_plugin time_plugin)
add_dependencies(test_regex      str_plugin regex_plugin)
add_dependencies(test_embrmeta   embrmeta_plugin)
add_dependencies(test_ffi_embr   ffi_module embrmeta_plugin embrtypes_plugin ffi_plugin regex_plugin str_plugin embrlib_plugin)
add_dependencies(test_table      table_plugin embrlib_plugin)
add_dependencies(test_vec        vec_plugin embrlib_plugin table_plugin json_plugin csv_plugin random_plugin)
add_dependencies(test_parallel   parallel_plugin embrlib_plugin table_plugin)
add_dependencies(test_parallel_embr parallel_module parallel_plugin embrlib_plugin)
add_dependencies(test_async      async_plugin embrlib_plugin table_plugin time_plugin)
add_dependencies(test_dunder     embrtypes_plugin json_plugin table_plugin embrlib_plugin ffi_plugin str_plugin)
add_dependencies(test_http       http_plugin)
add_dependencies(test_pkg_embr   pkg_module http_plugin json_plugin fs_plugin str_plugin embrlib_plugin)
add_dependencies(test_gc         gc_plugin table_plugin vec_plugin ffi_plugin embrtypes_plugin embrlib_plugin)
add_dependencies(test_coroutine  coroutine_plugin)

# end-to-end https:// test against a local TLS server (needs python3 + the openssl CLI; skips itself
# (exit 77) when either is missing or the http plugin was built without OpenSSL). shell-driven because
# the C++ harness can't host a server. registered per backend like every other suite.
find_program(BASH_EXE bash)
if(BASH_EXE AND NOT WIN32)
    set(https_work "${BIN_DIR}/test-work")
    if(EMBR_WITH_TREE_WALKER AND EMBR_WITH_VM)
        file(MAKE_DIRECTORY "${https_work}/https_tw" "${https_work}/https_vm")
        add_test(NAME https_tw COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/https_smoke.sh $<TARGET_FILE:embr>
                 WORKING_DIRECTORY "${https_work}/https_tw")
        add_test(NAME https_vm COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/https_smoke.sh $<TARGET_FILE:embr> --vm
                 WORKING_DIRECTORY "${https_work}/https_vm")
        set_tests_properties(https_tw https_vm PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        add_dependencies(embr http_plugin)
    else()
        file(MAKE_DIRECTORY "${https_work}/https")
        # a VM-only build has no tree-walker to fall back on, so it must be told to use the VM
        set(https_vm_flag "")
        if(EMBR_WITH_VM AND NOT EMBR_WITH_TREE_WALKER)
            set(https_vm_flag "--vm")
        endif()
        add_test(NAME https COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/https_smoke.sh $<TARGET_FILE:embr> ${https_vm_flag}
                 WORKING_DIRECTORY "${https_work}/https")
        set_tests_properties(https PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        add_dependencies(embr http_plugin)
    endif()
endif()

# the docs live in the GitHub wiki (the wiki/ submodule). the docs checks below read it through EMBR_WIKI_DIR, see the
# top-level CMakeLists. without a wiki they are skipped

# editor grammar (tools/editor/vscode): regexes compile, keyword list matches the wiki's lexical-grammar page, sample
# snippets and every repo .embr file lex cleanly under the grammar's rules
find_program(PYTHON3_EXE python3)
if(PYTHON3_EXE)
    add_test(NAME editor_grammar COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/editor/check_grammar.py)
    if(EMBR_WIKI_DIR)
        set_tests_properties(editor_grammar PROPERTIES ENVIRONMENT "EMBR_WIKI_DIR=${EMBR_WIKI_DIR}")
    endif()
endif()

if(PYTHON3_EXE)
    add_test(NAME lsp_smoke COMMAND ${PYTHON3_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/lsp_smoke.py $<TARGET_FILE:embr>)
endif()
# runs the tagged code blocks in the wiki pages on every backend, checks the numbers the wiki states, and checks
# that every plugin and module has a wiki page (skipped, exit 77, without EMBR_WIKI_DIR)
if(PYTHON3_EXE)
    add_test(NAME docs_examples COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/docs/check_wiki.py
             --embr ${BIN_DIR}/embr --include ${CMAKE_BINARY_DIR}/generated --wiki "${EMBR_WIKI_DIR}")
    set_tests_properties(docs_examples PROPERTIES SKIP_RETURN_CODE 77)
endif()

# differential test: the Pike-VM regex engine (plugins/regex/pike.h) vs std::regex on random patterns.
# standalone (no embr header / plugins needed), so it is a plain executable
add_executable(test_regex_engine regex_engine_test.cpp)
set_target_properties(test_regex_engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/tests")
add_test(NAME regex_engine_diff COMMAND test_regex_engine)
set_tests_properties(regex_engine_diff PROPERTIES SKIP_RETURN_CODE 77)

# the real `embr` binary on malformed / edge-case scripts: must report an error, never crash (see cli_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    if(EMBR_WITH_TREE_WALKER AND EMBR_WITH_VM)
        add_test(NAME cli_smoke_tw COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.sh $<TARGET_FILE:embr>)
        add_test(NAME cli_smoke_vm COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.sh $<TARGET_FILE:embr> --vm)
    elseif(EMBR_WITH_VM)
        add_test(NAME cli_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.sh $<TARGET_FILE:embr> --vm)
    else()
        add_test(NAME cli_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.sh $<TARGET_FILE:embr>)
    endif()
endif()

# `cmake --install` into a temp prefix, then run scripts from there: plugins and modules must be found (see install_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    add_test(NAME install_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/install_smoke.sh ${CMAKE_BINARY_DIR} ${CMAKE_PROJECT_VERSION})
endif()

# `embr fmt`: golden file, idempotence over every repo .embr file, --check (see fmt_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    add_test(NAME fmt_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/fmt_smoke.sh $<TARGET_FILE:embr> ${CMAKE_SOURCE_DIR})
endif()

# sandbox / plugin allow-list, including escape attempts (see sandbox_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    if(EMBR_WITH_TREE_WALKER AND EMBR_WITH_VM)
        add_test(NAME sandbox_smoke_tw COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/sandbox_smoke.sh $<TARGET_FILE:embr>)
        add_test(NAME sandbox_smoke_vm COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/sandbox_smoke.sh $<TARGET_FILE:embr> --vm)
    elseif(EMBR_WITH_VM)
        add_test(NAME sandbox_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/sandbox_smoke.sh $<TARGET_FILE:embr> --vm)
    else()
        add_test(NAME sandbox_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/sandbox_smoke.sh $<TARGET_FILE:embr>)
    endif()
endif()

# script tests: real embr test files (tests/script_tests/test_*.embr) run by `embr test`, dogfooding the runner
# and cross-checking core language behaviour on each backend
if(EMBR_WITH_TREE_WALKER AND EMBR_WITH_VM)
    file(MAKE_DIRECTORY "${BIN_DIR}/test-work/script_tests_tw" "${BIN_DIR}/test-work/script_tests_vm")
    add_test(NAME script_tests_tw COMMAND $<TARGET_FILE:embr> test ${CMAKE_CURRENT_SOURCE_DIR}/script_tests
             WORKING_DIRECTORY "${BIN_DIR}/test-work/script_tests_tw")
    add_test(NAME script_tests_vm COMMAND $<TARGET_FILE:embr> --vm test ${CMAKE_CURRENT_SOURCE_DIR}/script_tests
             WORKING_DIRECTORY "${BIN_DIR}/test-work/script_tests_vm")
    set_tests_properties(script_tests_tw script_tests_vm PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR}")
else()
    file(MAKE_DIRECTORY "${BIN_DIR}/test-work/script_tests")
    set(script_vm_flag "")
    if(EMBR_WITH_VM AND NOT EMBR_WITH_TREE_WALKER)
        set(script_vm_flag "--vm")
    endif()
    add_test(NAME script_tests COMMAND $<TARGET_FILE:embr> ${script_vm_flag} test ${CMAKE_CURRENT_SOURCE_DIR}/script_tests
             WORKING_DIRECTORY "${BIN_DIR}/test-work/script_tests")
    set_tests_properties(script_tests PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR}")
endif()
