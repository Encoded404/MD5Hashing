# Executable that links the static library
add_executable(${APP_TARGET})
target_sources(${APP_TARGET} PRIVATE
    main.cpp
    Timer.cpp
)

target_sources(${APP_TARGET} PRIVATE
    FILE_SET CXX_MODULES
    FILES
        Timer.cppm
)

target_link_libraries(${APP_TARGET} PRIVATE ${HASH_LIBRARY_TARGET})

# CLI parsing. CLI11 is a vcpkg dependency (see vcpkg.json); the `libcxx` preset
# builds it against libc++ through the x64-linux-libcxx overlay triplet, so its
# ABI matches the std module used by this executable.
find_package(CLI11 CONFIG REQUIRED)
target_link_libraries(${APP_TARGET} PRIVATE CLI11::CLI11)

# Optionally set executable output name; keep simple default
set_target_properties(${APP_TARGET} PROPERTIES OUTPUT_NAME ${APP_TARGET})

if(OUTPUT_PATH)
    set_target_properties(${APP_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_PATH})
endif()

# Set the C++ standard and enable compiler features
# (C++ standard is set globally in the top-level CMakeLists; this pins a floor)
target_compile_features(${APP_TARGET} PRIVATE cxx_std_26)

# Compiler-specific warning options for the executable target
target_compile_options(${APP_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${APP_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Add compile definitions for logging toggle
# Define LOGIFACE_ENABLE_LOGGING based on ENABLE_LOGGING
target_compile_definitions(${APP_TARGET}
    PUBLIC
        $<$<BOOL:${ENABLE_LOGGING}>:LOGIFACE_ENABLE_LOGGING=1>
        $<$<NOT:$<BOOL:${ENABLE_LOGGING}>>:LOGIFACE_ENABLE_LOGGING=0>
        LOGIFACE_DEFAULT_TO_FUNCTION=1
)

# Size/optimization glue (see src/HashLib/CMakeLists.txt for why it is per-target)
target_link_libraries(${APP_TARGET} PRIVATE project_sections project_size)
if(TARGET project_sanitizers)
    target_link_libraries(${APP_TARGET} PRIVATE project_sanitizers)
endif()

# Release deliverables ship without a symbol table: .symtab/.strtab carries
# nothing the program needs at runtime. Applied at link time so the table is
# never written; Debug/RelWithDebInfo keep their (compressed) DWARF.
target_link_options(${APP_TARGET} PRIVATE
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:Release>>:-Wl,--strip-all>
)

# On Windows/MinGW, place the runtime + dependency DLLs next to the executable.
deploy_runtime_dlls(${APP_TARGET})

if(CLANG_TIDY_ENABLED)
    target_clang_tidy(${APP_TARGET})
endif()
