# The anagram/preimage search library. It depends on HashLib for the batched
# MD5 path but knows nothing about the CLI; the app is a thin adapter.
add_library(${SOLVE_LIBRARY_TARGET} STATIC)
target_compile_features(${SOLVE_LIBRARY_TARGET} PUBLIC cxx_std_26)
target_link_libraries(${SOLVE_LIBRARY_TARGET} PUBLIC ${HASH_LIBRARY_TARGET})

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

# One module interface, one implementation unit. The interface is PUBLIC so the
# app and the tests can `import solve;`.
target_sources(${SOLVE_LIBRARY_TARGET} PUBLIC
    FILE_SET CXX_MODULES
    FILES
        solve.cppm
)

target_sources(${SOLVE_LIBRARY_TARGET} PRIVATE
    solve.cpp
)

target_link_libraries(${SOLVE_LIBRARY_TARGET} PRIVATE project_sections project_size)
if(TARGET project_sanitizers)
    target_link_libraries(${SOLVE_LIBRARY_TARGET} PRIVATE project_sanitizers)
endif()

if(CLANG_TIDY_ENABLED)
    target_clang_tidy(${SOLVE_LIBRARY_TARGET})
endif()
