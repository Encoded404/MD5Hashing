# Add the test executable
add_executable(example_tests example_tests.cpp)

# Find GTest installed via vcpkg and link it along with the main library
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
target_link_libraries(example_tests PRIVATE
    ${LIBRARY_TARGET}
    GTest::gtest
    GTest::gtest_main
)

# Set compiler options for the test executable
target_compile_options(example_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
	target_compile_options(example_tests PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Let CTest discover individual GoogleTest tests in the binary
gtest_discover_tests(example_tests
    PROPERTIES
        ENVIRONMENT "TEST_LOG_DIR=${CMAKE_BINARY_DIR}/Testing/logs"
)

# Same debug-info compression and gc-sections as the shipped binaries. Test
# executables are deliberately not stripped, so a failing test is always
# symbolizable.
target_link_libraries(example_tests PRIVATE project_sections project_size)
if(TARGET project_sanitizers)
    target_link_libraries(example_tests PRIVATE project_sanitizers)
endif()

# On Windows/MinGW, place the runtime + dependency DLLs next to the test binary
# so it can be run (e.g. under wine).
deploy_runtime_dlls(example_tests)

if(CLANG_TIDY_ENABLED)
    target_clang_tidy(example_tests)
endif()
