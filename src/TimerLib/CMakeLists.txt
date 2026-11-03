# Small shared stopwatch + duration-formatting utility. Split into its own
# library so both SolveLib (phase timing) and the CLI (benchmarks, reports) can
# import `Timer` without either one owning the other.
add_library(${TIMER_LIBRARY_TARGET} STATIC)
target_compile_features(${TIMER_LIBRARY_TARGET} PUBLIC cxx_std_26)

target_compile_options(${TIMER_LIBRARY_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${TIMER_LIBRARY_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# PUBLIC so the app and anything else that imports `Timer` can see the module.
target_sources(${TIMER_LIBRARY_TARGET} PUBLIC
    FILE_SET CXX_MODULES
    FILES
        Timer.cppm
)

target_sources(${TIMER_LIBRARY_TARGET} PRIVATE
    Timer.cpp
)

target_link_libraries(${TIMER_LIBRARY_TARGET} PRIVATE project_sections project_size)
if(TARGET project_sanitizers)
    target_link_libraries(${TIMER_LIBRARY_TARGET} PRIVATE project_sanitizers)
endif()

if(CLANG_TIDY_ENABLED)
    target_clang_tidy(${TIMER_LIBRARY_TARGET})
endif()
