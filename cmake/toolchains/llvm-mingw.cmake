# =============================================================================
# Cross-compile toolchain: Linux (x86_64) -> Windows (x86_64) using the
# llvm-mingw distribution.
#
# llvm-mingw is a self-contained LLVM/Clang/LLD based mingw-w64 toolchain that
# ships clang, lld, compiler-rt, libunwind, libc++/libc++abi and a complete
# mingw-w64 sysroot (headers + CRT + import libraries), including the libc++
# standard-library module metadata (libc++.modules.json + std.cppm) required
# for `import std;`.
#
# Download: https://github.com/mstorsjo/llvm-mingw/releases
#   llvm-mingw-<version>-ucrt-ubuntu-22.04-x86_64.tar.xz
#
# Extract it into the project as <project>/toolchains/ (that directory is
# gitignored), then configure with `cmake --preset windows`. Full instructions,
# including the exact download commands, are in
# docs/cross-compiling-windows.md.
#
# This file is chainloaded in two places:
#   * from the `x64-mingw-libcxx` vcpkg triplet, which applies it to every
#     vcpkg port build; and
#   * from the `windows` configure preset via VCPKG_CHAINLOAD_TOOLCHAIN_FILE,
#     which applies it to the consuming project. vcpkg.cmake only honours that
#     variable when it is visible during the consumer configure — a value set
#     in the triplet alone is NOT propagated to the consumer.
#
# The compiler executables used are llvm-mingw's per-target *wrapper scripts*
# (e.g. bin/x86_64-w64-mingw32-clang++). They inject `-target x86_64-w64-mingw32`
# and read the checked-in `.cfg` files, which add
#   -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++ -fuse-ld=lld
# Using the wrappers (rather than setting CMAKE_*_COMPILER_TARGET ourselves)
# also makes CMake derive the toolchain prefix `x86_64-w64-mingw32-`, so it
# automatically picks up `x86_64-w64-mingw32-clang-scan-deps`, `llvm-ar`, etc.
#
# The toolchain root is taken from -DLLVM_MINGW_ROOT=<dir> or the
# LLVM_MINGW_ROOT environment variable, falling back to auto-detection.
# =============================================================================

if(NOT _TEMPLATE_LLVM_MINGW_TOOLCHAIN)
    set(_TEMPLATE_LLVM_MINGW_TOOLCHAIN 1)

    # Guard against a stale build tree. CMake only (re)generates
    # CMakeFiles/<ver>/CMakeSystem.cmake when the target system changes, so a
    # build directory first configured *before* this cross toolchain was wired
    # in keeps a CMakeSystem.cmake that reports a native system and
    # CMAKE_CROSSCOMPILING=FALSE. That silently disables all cross-specific
    # logic while the compiler is nevertheless the cross compiler. Fail loudly
    # instead.
    if(CMAKE_SYSTEM_LOADED AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        message(FATAL_ERROR
            "Stale CMake build tree: this build directory records target system "
            "'${CMAKE_SYSTEM_NAME}' (from CMakeFiles/*/CMakeSystem.cmake) instead of "
            "'Windows', so it predates the llvm-mingw cross toolchain and has "
            "CMAKE_CROSSCOMPILING=FALSE. Delete the build directory "
            "(e.g. `rm -rf build-windows`) and configure again.")
    endif()

    if(POLICY CMP0056)
        cmake_policy(SET CMP0056 NEW)
    endif()
    if(POLICY CMP0066)
        cmake_policy(SET CMP0066 NEW)
    endif()
    if(POLICY CMP0067)
        cmake_policy(SET CMP0067 NEW)
    endif()
    if(POLICY CMP0137)
        cmake_policy(SET CMP0137 NEW)
    endif()

    # vcpkg passes its triplet variables through try_compile; keep that working.
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
        VCPKG_CRT_LINKAGE VCPKG_TARGET_ARCHITECTURE
        VCPKG_C_FLAGS VCPKG_CXX_FLAGS
        VCPKG_C_FLAGS_DEBUG VCPKG_CXX_FLAGS_DEBUG
        VCPKG_C_FLAGS_RELEASE VCPKG_CXX_FLAGS_RELEASE
        VCPKG_LINKER_FLAGS VCPKG_LINKER_FLAGS_RELEASE VCPKG_LINKER_FLAGS_DEBUG
    )

    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(CMAKE_CROSSCOMPILING OFF CACHE BOOL "")
    endif()

    # VCPKG_CMAKE_SYSTEM_NAME is "MinGW" for this triplet, but CMake has no
    # MinGW system name; force the real one (same as vcpkg's mingw.cmake).
    set(CMAKE_SYSTEM_NAME Windows CACHE STRING "" FORCE)

    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
        set(CMAKE_SYSTEM_PROCESSOR i686 CACHE STRING "")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm")
        set(CMAKE_SYSTEM_PROCESSOR armv7 CACHE STRING "")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(CMAKE_SYSTEM_PROCESSOR aarch64 CACHE STRING "")
    else()
        set(CMAKE_SYSTEM_PROCESSOR x86_64 CACHE STRING "")
    endif()

    # ---- Locate the llvm-mingw installation -----------------------------
    # Search order:
    #   1. -DLLVM_MINGW_ROOT=<dir> (or the same environment variable)
    #   2. <project>/toolchains/llvm-mingw*  (recommended: keep it in-tree)
    #   3. ~/opt/llvm-mingw*, ~/llvm-mingw*, /opt/llvm-mingw*
    #
    # Option 2 is what docs/cross-compiling-windows.md describes: download and
    # extract the release into <project>/toolchains/, which .gitignore excludes.

    # A cached root that no longer exists (e.g. the toolchain was moved into
    # toolchains/) must not stick; drop it and re-detect.
    if(LLVM_MINGW_ROOT AND
       NOT IS_DIRECTORY "${LLVM_MINGW_ROOT}/bin")
        message(STATUS
            "LLVM_MINGW_ROOT='${LLVM_MINGW_ROOT}' is no longer a valid llvm-mingw "
            "directory; searching again.")
        unset(LLVM_MINGW_ROOT)
        unset(LLVM_MINGW_ROOT CACHE)
    endif()

    if(NOT LLVM_MINGW_ROOT)
        if(DEFINED ENV{LLVM_MINGW_ROOT})
            set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
        else()
            get_filename_component(_tpl_project_root
                "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
            file(GLOB _tpl_llvm_mingw_candidates LIST_DIRECTORIES true
                "${_tpl_project_root}/toolchains/llvm-mingw*"
                "$ENV{HOME}/opt/llvm-mingw-*"
                "$ENV{HOME}/llvm-mingw-*"
                "/opt/llvm-mingw-*"
            )
            # GLOB also matches downloaded .tar.xz files; keep only directories.
            foreach(_tpl_candidate IN LISTS _tpl_llvm_mingw_candidates)
                if(IS_DIRECTORY "${_tpl_candidate}")
                    list(APPEND _tpl_llvm_mingw_dirs "${_tpl_candidate}")
                endif()
            endforeach()
            # The project-local candidate is listed first, so it wins.
            if(_tpl_llvm_mingw_dirs)
                list(GET _tpl_llvm_mingw_dirs 0 LLVM_MINGW_ROOT)
            endif()
            unset(_tpl_llvm_mingw_candidates)
            unset(_tpl_llvm_mingw_dirs)
            unset(_tpl_project_root)
        endif()
    endif()
    set(LLVM_MINGW_ROOT "${LLVM_MINGW_ROOT}" CACHE PATH
        "Root of the llvm-mingw toolchain (containing bin/, share/, <triple>/)")

    if(NOT LLVM_MINGW_ROOT OR
       NOT EXISTS "${LLVM_MINGW_ROOT}/bin")
        message(FATAL_ERROR
            "llvm-mingw not found. Download a release from "
            "https://github.com/mstorsjo/llvm-mingw/releases and extract it into "
            "<project>/toolchains/ (see docs/cross-compiling-windows.md), or set "
            "-DLLVM_MINGW_ROOT=<dir> / the LLVM_MINGW_ROOT environment variable.")
    endif()

    set(_tpl_mingw_prefix "${CMAKE_SYSTEM_PROCESSOR}-w64-mingw32")
    set(_tpl_mingw_bin "${LLVM_MINGW_ROOT}/bin")

    # The per-target wrapper scripts own the --target/-stdlib/-rtlib flags.
    # FORCE is deliberate: a cross toolchain is authoritative for the compiler
    # choice, and it must also win over a stale CMAKE_CXX_COMPILER already
    # recorded in the build directory's CMakeCache.
    set(CMAKE_C_COMPILER   "${_tpl_mingw_bin}/${_tpl_mingw_prefix}-clang"
        CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER "${_tpl_mingw_bin}/${_tpl_mingw_prefix}-clang++"
        CACHE FILEPATH "" FORCE)
    set(CMAKE_ASM_COMPILER "${_tpl_mingw_bin}/${_tpl_mingw_prefix}-clang"
        CACHE FILEPATH "" FORCE)
    set(CMAKE_RC_COMPILER  "${_tpl_mingw_bin}/${_tpl_mingw_prefix}-windres"
        CACHE FILEPATH "" FORCE)

    set(CMAKE_AR      "${_tpl_mingw_bin}/llvm-ar"      CACHE FILEPATH "" FORCE)
    set(CMAKE_RANLIB  "${_tpl_mingw_bin}/llvm-ranlib"  CACHE FILEPATH "" FORCE)
    set(CMAKE_STRIP   "${_tpl_mingw_bin}/llvm-strip"   CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJCOPY "${_tpl_mingw_bin}/llvm-objcopy" CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJDUMP "${_tpl_mingw_bin}/llvm-objdump" CACHE FILEPATH "" FORCE)
    set(CMAKE_NM      "${_tpl_mingw_bin}/llvm-nm"      CACHE FILEPATH "" FORCE)

    # Module dependency scanner (used for C++26 named modules and import std).
    set(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS
        "${_tpl_mingw_bin}/${_tpl_mingw_prefix}-clang-scan-deps"
        CACHE FILEPATH "" FORCE)

    # ---- import std: point CMake at the *target* libc++ metadata ----------
    # CMake >= 4.2 honours CMAKE_CXX_STDLIB_MODULES_JSON and skips its own
    # compiler probe (`clang++ -print-file-name=libc++.modules.json`), which
    # would otherwise resolve the host's libc++ metadata during a cross build.
    set(_tpl_libcxx_modules_json
        "${LLVM_MINGW_ROOT}/${_tpl_mingw_prefix}/lib/libc++.modules.json")
    if(EXISTS "${_tpl_libcxx_modules_json}")
        set(CMAKE_CXX_STDLIB_MODULES_JSON "${_tpl_libcxx_modules_json}"
            CACHE FILEPATH "Standard-library module metadata for the target libc++")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL 4.2)
        message(WARNING
            "libc++ module metadata not found at ${_tpl_libcxx_modules_json}; "
            "`import std;` will not be available for the Windows target.")
    endif()

    if(CMAKE_VERSION VERSION_LESS 4.2)
        message(WARNING
            "Cross-compiling this project with `import std;` requires CMake >= 4.2 "
            "(CMAKE_CXX_STDLIB_MODULES_JSON). Detected CMake ${CMAKE_VERSION}; "
            "use the CMake bundled with your IDE if it is newer.")
    endif()

    # ---- vcpkg flag plumbing (mirrors vcpkg's scripts/toolchains/mingw.cmake) ----
    string(APPEND CMAKE_C_FLAGS_INIT " ${VCPKG_C_FLAGS} ")
    string(APPEND CMAKE_CXX_FLAGS_INIT " ${VCPKG_CXX_FLAGS} ")
    string(APPEND CMAKE_C_FLAGS_DEBUG_INIT " ${VCPKG_C_FLAGS_DEBUG} ")
    string(APPEND CMAKE_CXX_FLAGS_DEBUG_INIT " ${VCPKG_CXX_FLAGS_DEBUG} ")
    string(APPEND CMAKE_C_FLAGS_RELEASE_INIT " ${VCPKG_C_FLAGS_RELEASE} ")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE_INIT " ${VCPKG_CXX_FLAGS_RELEASE} ")

    string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " ${VCPKG_LINKER_FLAGS} ")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " ${VCPKG_LINKER_FLAGS} ")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " ${VCPKG_LINKER_FLAGS} ")
    if(VCPKG_CRT_LINKAGE STREQUAL "static")
        string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT "-static ")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT "-static ")
        string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT "-static ")
    endif()
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_DEBUG_INIT " ${VCPKG_LINKER_FLAGS_DEBUG} ")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_DEBUG_INIT " ${VCPKG_LINKER_FLAGS_DEBUG} ")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_DEBUG_INIT " ${VCPKG_LINKER_FLAGS_DEBUG} ")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_RELEASE_INIT " ${VCPKG_LINKER_FLAGS_RELEASE} ")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE_INIT " ${VCPKG_LINKER_FLAGS_RELEASE} ")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT " ${VCPKG_LINKER_FLAGS_RELEASE} ")
endif()
