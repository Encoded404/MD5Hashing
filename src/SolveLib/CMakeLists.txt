# The anagram/preimage search library. It depends on HashLib for the batched
# MD5 path but knows nothing about the CLI; the app is a thin adapter.
add_library(${SOLVE_LIBRARY_TARGET} STATIC)
target_compile_features(${SOLVE_LIBRARY_TARGET} PUBLIC cxx_std_26)
target_link_libraries(${SOLVE_LIBRARY_TARGET}
    PUBLIC ${HASH_LIBRARY_TARGET}
    PRIVATE ${TIMER_LIBRARY_TARGET}
)

# Per-phase solve timing is a diagnostic: it reads the clock once per phase and
# keeps a handful of counters. Compile it in only for Debug and RelWithDebInfo;
# Release runs the same search loop as an uninstrumented build. PUBLIC so the
# CLI, which formats the report, sees the same setting.
target_compile_definitions(${SOLVE_LIBRARY_TARGET} PUBLIC
    "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:SOLVE_TIMING=1>"
)

target_compile_options(${SOLVE_LIBRARY_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${SOLVE_LIBRARY_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# The public `solve` interface, the vocabulary module it re-exports, and an internal `solve.detail` module holding declarations the implementation units share.
# All three are in the file set because CMake needs every module interface listed; consumers only import `solve`.
target_sources(${SOLVE_LIBRARY_TARGET} PUBLIC
    FILE_SET CXX_MODULES
    FILES
        solve.cppm
        solve.types.cppm
        solve.detail.cppm
)

# One unit per concern: the public API glue, wordlist I/O, the candidate sources
# and hash driver, each phrase engine, and planning.
target_sources(${SOLVE_LIBRARY_TARGET} PRIVATE
    solve.cpp
    wordlist.cpp
    engine.cpp
    phrase.cpp
    phrase_v1.cpp
    plan.cpp
)

target_link_libraries(${SOLVE_LIBRARY_TARGET} PRIVATE project_sections project_size)
if(TARGET project_sanitizers)
    target_link_libraries(${SOLVE_LIBRARY_TARGET} PRIVATE project_sanitizers)
endif()

if(CLANG_TIDY_ENABLED)
    target_clang_tidy(${SOLVE_LIBRARY_TARGET})
endif()
