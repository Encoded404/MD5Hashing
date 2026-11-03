# Hash library
add_subdirectory(HashLib)

# Shared stopwatch and duration formatting, used by SolveLib and the CLI
add_subdirectory(TimerLib)

# Anagram/preimage search library on top of HashLib
add_subdirectory(SolveLib)

# CLI that links the static libraries
add_subdirectory(app)
