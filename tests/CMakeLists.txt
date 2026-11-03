# Add the test executable
add_executable(hashlib_tests hashlib_tests.cpp solve_tests.cpp)

# Test-only support module. GTest's headers stay in its global module fragment;
# the test TU includes them before `import std;` (see hashlib_tests.cpp).
# Compiled per target, matching VulkanEngineV5's layout, so multiple test
# executables cannot disagree about the module BMI.
target_sources(hashlib_tests PRIVATE
    FILE_SET CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES
        test_logging.cppm
)

target_compile_features(hashlib_tests PRIVATE cxx_std_26)

# Find GTest installed via vcpkg and link it along with the main library.
# GTest::gtest puts the gtest include directory on the `-isystem` path, which
# keeps its C++26 warnings out of this project's `-Werror` build.
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
target_link_libraries(hashlib_tests PRIVATE
    ${HASH_LIBRARY_TARGET}
    ${SOLVE_LIBRARY_TARGET}
    GTest::gtest
    GTest::gtest_main
)

# Set compiler options for the test executable
target_compile_options(hashlib_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
	target_compile_options(hashlib_tests PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Let CTest discover individual GoogleTest tests in the binary
gtest_discover_tests(hashlib_tests
    PROPERTIES
        ENVIRONMENT "TEST_LOG_DIR=${CMAKE_BINARY_DIR}/Testing/logs"
)

# End-to-end CLI smoke tests for `solve`.
# The answer is the only thing written to stdout; the logger writes to std::clog.
# A match here therefore means the result really came from the solver, not from a log line.
add_test(NAME solve_cli_digest
         COMMAND $<TARGET_FILE:${APP_TARGET}> solve
                 --digest 839d7228b5ccc60e54455093a966b8c2
                 --anagram-of silent)
set_tests_properties(solve_cli_digest PROPERTIES PASS_REGULAR_EXPRESSION "listen")

add_test(NAME solve_cli_same_as
         COMMAND $<TARGET_FILE:${APP_TARGET}> solve
                 --same-as listen --anagram-of silent)
set_tests_properties(solve_cli_same_as PROPERTIES PASS_REGULAR_EXPRESSION "listen")

add_test(NAME solve_cli_phrase
         COMMAND $<TARGET_FILE:${APP_TARGET}> solve
                 --digest 7533f6c3cf1f2e0b2667462c5518966f
                 --anagram-of "silent to me"
                 --wordlist ${CMAKE_CURRENT_SOURCE_DIR}/data/phrase-words.txt
                 --solver phrase)
set_tests_properties(solve_cli_phrase PROPERTIES PASS_REGULAR_EXPRESSION "listen to me")

# A one-permutation space that cannot match.
# The CLI must exit non-zero rather than print a candidate.
add_test(NAME solve_cli_no_match
         COMMAND $<TARGET_FILE:${APP_TARGET}> solve
                 --digest 839d7228b5ccc60e54455093a966b8c2
                 --anagram-of aaa)
set_tests_properties(solve_cli_no_match PROPERTIES WILL_FAIL TRUE)

# The batch dispatcher selects the widest kernel the CPU supports and caches it,
# so the run above only ever executes one compiled kernel. Run the batch tests
# once per kernel through the dispatcher's own HASHLIB_FORCE_ISA hook. When the
# named kernel is unavailable on this CPU the dispatcher falls back to the
# scalar path, and Md5BatchUsesForcedKernel then reports SKIPPED instead of
# passing without exercising anything.
if(HASHLIB_SIMD)
    foreach(isa IN ITEMS sse2 avx2 avx512f scalar)
        add_test(NAME hashlib_tests_batch_${isa}
                 COMMAND $<TARGET_FILE:hashlib_tests> --gtest_filter=HashLibTest.Md5Batch*)
        set_tests_properties(hashlib_tests_batch_${isa} PROPERTIES
            ENVIRONMENT "HASHLIB_FORCE_ISA=${isa};TEST_LOG_DIR=${CMAKE_BINARY_DIR}/Testing/logs")
    endforeach()
endif()

# Same debug-info compression and gc-sections as the shipped binaries. Test
# executables are deliberately not stripped, so a failing test is always
# symbolizable.
target_link_libraries(hashlib_tests PRIVATE project_sections project_size)
if(TARGET project_sanitizers)
    target_link_libraries(hashlib_tests PRIVATE project_sanitizers)
endif()

# On Windows/MinGW, place the runtime + dependency DLLs next to the test binary
# so it can be run (e.g. under wine).
deploy_runtime_dlls(hashlib_tests)

if(CLANG_TIDY_ENABLED)
    target_clang_tidy(hashlib_tests)
endif()
