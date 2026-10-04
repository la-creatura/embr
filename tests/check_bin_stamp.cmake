# fails when bin/<platform>/ was last written by a different build directory than the one running ctest
# (the shared bin/ folder, see README.md). cmake -DSTAMP=<file> -DEXPECTED=<build dir name> -P check_bin_stamp.cmake
if(NOT EXISTS "${STAMP}")
    message(FATAL_ERROR "${STAMP} is missing: build this preset first (cmake --build build/${EXPECTED})")
endif()
file(READ "${STAMP}" built_by)
string(STRIP "${built_by}" built_by)
if(NOT built_by STREQUAL EXPECTED)
    message(FATAL_ERROR
        "bin/ was last built by '${built_by}', but these tests belong to '${EXPECTED}'. bin/ is shared by every "
        "preset, so another build overwrote it. rebuild this one: "
        "rm -f bin/linux/embr bin/linux/tests/test_* bin/linux/plugins/*.so && cmake --build build/${EXPECTED}")
endif()
