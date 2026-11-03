# Hash library
add_subdirectory(HashLib)

# Anagram/preimage search library on top of HashLib
add_subdirectory(SolveLib)

# CLI that links the static libraries
add_subdirectory(app)
