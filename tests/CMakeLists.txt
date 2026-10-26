# Add the test executable
add_executable(hashlib_tests hashlib_tests.cpp)

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
