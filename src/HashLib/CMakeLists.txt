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

# Optional per-machine tuning (HASHLIB_NATIVE_TUNING).
#
# -mtune=generic is pinned next to -march=native because on GCC -march=native
# implies -mtune=znver5, which measured 20% slower for the MD5 core before the
# Pin barrier removed the tuning sensitivity (see docs/md5-optimization.md).
# Skipped when cross-compiling, where -march=native would describe the build
# host rather than the target.
#
# The flags go on an INTERFACE target linked PUBLIC rather than on the library
# PRIVATE, because they have to reach every translation unit that imports
# `hashlib`. A Clang module BMI records the target features it was built with,
# and importing it from a TU with a different feature set is a hard error
# ("precompiled file ... was compiled with the target feature '+avx512f' but the
# current translation unit is not"). The interface target carries the options to
# HashApp and hashlib_tests through the library's usage requirements, which is
# what makes this option buildable with Clang.
if(HASHLIB_NATIVE_TUNING AND NOT CMAKE_CROSSCOMPILING)
    add_library(hashlib_arch_flags INTERFACE)
    target_compile_options(hashlib_arch_flags INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-march=native>
        $<$<CXX_COMPILER_ID:GNU,Clang>:-mtune=generic>
    )
    target_link_libraries(${HASH_LIBRARY_TARGET} PUBLIC hashlib_arch_flags)
endif()

# Module files.
# PUBLIC so consumers (the app, the tests) can `import hashlib;` and
# `import hashlib.utilities;`. batch/batch.cppm is the internal batch module;
# it is in this file set because CMake needs every module interface listed
# here, but consumers do not import it.
target_sources(${HASH_LIBRARY_TARGET} PUBLIC
    FILE_SET CXX_MODULES
    FILES
        hashlib.cppm
        utilities.cppm
        batch/batch.cppm
)

# Source files
target_sources(${HASH_LIBRARY_TARGET} PRIVATE
    hashlib.cpp
    utilities.cpp
    batch/hash_many.cpp
    batch/dispatch.cpp
)

# ── Batched SIMD kernels ────────────────────────────────────────────────────
# The dispatcher is always compiled for the baseline ISA. Every kernel lives in
# its own TU with exactly the flags for that ISA, so the build machine's CPU is
# irrelevant: only the compiler's ability to *emit* the instructions matters.
# md5_batch chooses among them on the CPU that runs the binary.
if(HASHLIB_SIMD)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _hashlib_processor)
    if(_hashlib_processor MATCHES "^(x86_64|amd64)$")
        find_package(xsimd CONFIG REQUIRED)
        if(TARGET xsimd::xsimd)
            target_link_libraries(${HASH_LIBRARY_TARGET} PRIVATE xsimd::xsimd)
        else()
            target_link_libraries(${HASH_LIBRARY_TARGET} PRIVATE xsimd)
        endif()

        # Lets the dispatcher switch on one macro instead of repeating the
        # four compiler architecture spellings in every #if.
        target_compile_definitions(${HASH_LIBRARY_TARGET} PRIVATE HASHLIB_X86=1)
        target_sources(${HASH_LIBRARY_TARGET} PRIVATE batch/kernel_sse2.cpp)

        include(CheckCXXCompilerFlag)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            check_cxx_compiler_flag("-mavx2" HASHLIB_COMPILER_HAS_AVX2)
            check_cxx_compiler_flag("-mavx512f" HASHLIB_COMPILER_HAS_AVX512F)
        elseif(MSVC)
            set(HASHLIB_COMPILER_HAS_AVX2 TRUE)
            set(HASHLIB_COMPILER_HAS_AVX512F TRUE)
        else()
            set(HASHLIB_COMPILER_HAS_AVX2 FALSE)
            set(HASHLIB_COMPILER_HAS_AVX512F FALSE)
        endif()

        if(HASHLIB_COMPILER_HAS_AVX2)
            target_sources(${HASH_LIBRARY_TARGET} PRIVATE batch/kernel_avx2.cpp)
            target_compile_definitions(${HASH_LIBRARY_TARGET} PRIVATE HASHLIB_WITH_AVX2=1)
        endif()
        if(HASHLIB_COMPILER_HAS_AVX2 AND HASHLIB_COMPILER_HAS_AVX512F)
            target_sources(${HASH_LIBRARY_TARGET} PRIVATE batch/kernel_avx512.cpp)
            target_compile_definitions(${HASH_LIBRARY_TARGET} PRIVATE HASHLIB_WITH_AVX512=1)
        endif()

        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            # Pin everything that is not a per-ISA kernel to the x86-64
            # baseline. Otherwise a distro compiler default (or a future
            # default) could emit AVX in the dispatcher, and the binary would
            # fault before it could choose a kernel.
            set_source_files_properties(
                hashlib.cpp hashlib.cppm utilities.cpp utilities.cppm
                batch/batch.cppm batch/hash_many.cpp batch/dispatch.cpp
                batch/kernel_sse2.cpp
                PROPERTIES COMPILE_OPTIONS "-march=x86-64")
            if(HASHLIB_COMPILER_HAS_AVX2)
                # Pin the baseline explicitly *in addition to* the ISA flag:
                # with HASHLIB_NATIVE_TUNING a target-wide -march=native would
                # otherwise leave AVX-512 enabled in the AVX2 kernel (VEX/EVEX
                # 256-bit vprold/vpternlog), which faults on an AVX2-only CPU.
                set_source_files_properties(batch/kernel_avx2.cpp
                    PROPERTIES COMPILE_OPTIONS "-march=x86-64;-mavx2")
            endif()
            if(HASHLIB_COMPILER_HAS_AVX2 AND HASHLIB_COMPILER_HAS_AVX512F)
                set_source_files_properties(batch/kernel_avx512.cpp
                    PROPERTIES COMPILE_OPTIONS "-march=x86-64;-mavx2;-mavx512f")
            endif()
        elseif(MSVC)
            if(HASHLIB_COMPILER_HAS_AVX2)
                set_source_files_properties(batch/kernel_avx2.cpp
                    PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
            endif()
            if(HASHLIB_COMPILER_HAS_AVX2 AND HASHLIB_COMPILER_HAS_AVX512F)
                set_source_files_properties(batch/kernel_avx512.cpp
                    PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
            endif()
        endif()

        set(_hashlib_kernels "sse2")
        if(HASHLIB_COMPILER_HAS_AVX2)
            list(APPEND _hashlib_kernels "avx2")
        endif()
        if(HASHLIB_COMPILER_HAS_AVX2 AND HASHLIB_COMPILER_HAS_AVX512F)
            list(APPEND _hashlib_kernels "avx512f")
        endif()
        message(STATUS "HashLib SIMD kernels: ${_hashlib_kernels}")
    else()
        message(STATUS
            "HashLib SIMD: no kernels for ${CMAKE_SYSTEM_PROCESSOR}; md5_batch uses the scalar path")
    endif()
endif()

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
