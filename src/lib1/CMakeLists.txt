# Set the C++ standard and enable compiler features
add_library(${LIBRARY_TARGET} STATIC)
# The C++ standard is set globally in the top-level CMakeLists (C++23 with
# USE_CXX_MODULES, C++20 otherwise); we still pin a floor of C++20 here.
target_compile_features(${LIBRARY_TARGET} PUBLIC cxx_std_20)
target_link_libraries(${LIBRARY_TARGET} PUBLIC logiface)

# Compiler-specific warning options for the library target
target_compile_options(${LIBRARY_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${LIBRARY_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Module files.
# PUBLIC so consumers (the app, the tests) can `import lib_example;`.
target_sources(${LIBRARY_TARGET} PUBLIC
    FILE_SET CXX_MODULES
    FILES
        lib_example.cppm
)

# Source files
target_sources(${LIBRARY_TARGET} PRIVATE
    lib_example.cpp
)

# Size/optimization glue: gc-sections, debug-info compression and (Debug only)
# sanitizers. Compile options do not cross a static-library boundary, so every
# target that compiles its own sources links these interface targets directly.
target_link_libraries(${LIBRARY_TARGET} PRIVATE project_sections project_size)
if(TARGET project_sanitizers)
    target_link_libraries(${LIBRARY_TARGET} PRIVATE project_sanitizers)
endif()

if(CLANG_TIDY_ENABLED)
    target_clang_tidy(${LIBRARY_TARGET})
endif()
