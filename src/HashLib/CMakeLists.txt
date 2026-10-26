# Set the C++ standard and enable compiler features
add_library(${HASH_LIBRARY_TARGET} STATIC)
target_compile_features(${HASH_LIBRARY_TARGET} PUBLIC cxx_std_26)
target_link_libraries(${HASH_LIBRARY_TARGET} PUBLIC logiface)

# Compiler-specific warning options for the library target
target_compile_options(${HASH_LIBRARY_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${HASH_LIBRARY_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Module files.
# PUBLIC so consumers (the app, the tests) can `import hashlib;` and
# `import hashlib.utilities;`.
target_sources(${HASH_LIBRARY_TARGET} PUBLIC
    FILE_SET CXX_MODULES
    FILES
        hashlib.cppm
        utilities.cppm
)

# Source files
target_sources(${HASH_LIBRARY_TARGET} PRIVATE
    hashlib.cpp
    utilities.cpp
)

# Size/optimization glue: gc-sections, debug-info compression and (Debug only)
# sanitizers. Compile options do not cross a static-library boundary, so every
# target that compiles its own sources links these interface targets directly.
target_link_libraries(${HASH_LIBRARY_TARGET} PRIVATE project_sections project_size)
if(TARGET project_sanitizers)
    target_link_libraries(${HASH_LIBRARY_TARGET} PRIVATE project_sanitizers)
endif()

if(CLANG_TIDY_ENABLED)
    target_clang_tidy(${HASH_LIBRARY_TARGET})
endif()
