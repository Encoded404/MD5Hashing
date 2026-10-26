cmake_minimum_required(VERSION 3.30)

# Enable CMake's std-module support (`import std;`).
#
# `import std;` is still gated behind a per-release activation value on CMake
# 3.30-4.4. CMake deliberately changes that value between releases, so each
# verified band is listed explicitly. CMake >= 4.5 no longer needs the gate.
# This must run before project(), because the CXX toolchain probe records
# whether std-module support is available.
if( NOT CMAKE_EXPERIMENTAL_CXX_IMPORT_STD )
	if( CMAKE_VERSION VERSION_GREATER_EQUAL 4.5 )
		# CMake >= 4.5 enables import std without the experimental feature gate.
	elseif( CMAKE_VERSION VERSION_GREATER_EQUAL 4.4 )
		set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
	elseif( CMAKE_VERSION VERSION_GREATER_EQUAL 4.3 )
		set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
	elseif( CMAKE_VERSION VERSION_GREATER_EQUAL 3.30 AND CMAKE_VERSION VERSION_LESS 4.3 )
		# 3.30 through 4.2 use the same activation value (verified against
		# CMake 3.31.11 and the CMake 4.2 experimental-features guide).
		set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
	else()
		message(FATAL_ERROR
			"CMake ${CMAKE_VERSION} is outside the verified range for `import std;`. "
			"Use CMake 3.30-4.2, 4.3, 4.4, or >= 4.5. Alternatively set "
			"CMAKE_EXPERIMENTAL_CXX_IMPORT_STD to the value documented for "
			"CxxImportStd in this CMake's Help/dev/experimental.rst.")
	endif()
endif()

set(PROJECT_NAME "HashApp")
set(HASH_LIBRARY_TARGET "HashLib")
set(APP_TARGET "HashApp")

# if this is not set by the user cmake will decide
set(OUTPUT_PATH "${CMAKE_BINARY_DIR}/bin")

# `USE_LIBCPP`: build against libc++ instead of libstdc++ (Clang only).
#   libc++ ships a proper module map (libc++.modules.json) and is the
#   recommended standard library for `import std;`. There is no header fallback:
#   if this is ON and libc++ is unavailable, configuration fails.
#   NOTE: when ON, the vcpkg dependency tree must also be built against libc++,
#   otherwise ABI mismatches occur. That's what the `libcxx` preset does via the
#   overlay triplet cmake/triplets/x64-linux-libcxx.cmake.
option(USE_LIBCPP "Use libc++ instead of libstdc++ (Clang only)" OFF)

set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "Use standard C++" FORCE)

# ---- IMPORTANT: stdlib flags must be set BEFORE project() ----
#
# CMake's built-in std-module detection runs during project(), where it
# discovers the compiler frontend variant and the standard-library module
# source to use for `import std;`.
#
# On Fedora with Clang, the /etc/clang/x86_64-redhat-linux-gnu-clang++.cfg
# file passes `--gcc-triple=x86_64-redhat-linux`. This makes the compiler
# (and CMake's module scanner) find libstdc++'s bits/std.cc even when
# -stdlib=libc++ is specified, because --gcc-install-dir still points at the
# GCC installation. Pushing -stdlib=libc++ into the *initial* flags here makes
# CMake's probe and module detection see a Clang that uses libc++ everywhere.
if(USE_LIBCPP)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

# C++26 is mandatory: every first-party translation unit uses `import std;`,
# and there is no classic header-based fallback.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Set before project() so CMake's std-module detection is active while the
# compiler (and its std module files) are being probed.
set(CMAKE_CXX_MODULE_STD ON)

# ── Strip ccache shim directories from PATH before compiler detection ──
# Distros like Fedora put the ccache wrapper directory (e.g. /usr/lib64/ccache)
# first in PATH, so a bare `clang++`/`g++`/`c++` resolves to the ccache shim.
# ccache's cache key does not cover C++ module BMI/IFC contents, so after a
# module interface changes it can return objects compiled against the stale
# interface (ODR corruption). CCACHE_DISABLE=1 is NOT enough — ccache runs at
# build time, not configure time — so the only real fix is to make CMake detect
# and use the *real* compiler binary. This block also excludes *installed*
# candidates, so even a compiler configured inside a ccache dir is skipped.
# Compiler detection runs inside project(), so sanitize PATH here. This is a
# no-op on systems without ccache shims; the guard after project() still warns
# about any explicit launcher/compiler that resolves to ccache.
if(DEFINED ENV{PATH})
	if(CMAKE_HOST_WIN32)
		set(_ccache_path_sep ";")
	else()
		set(_ccache_path_sep ":")
	endif()
	string(REPLACE "${_ccache_path_sep}" ";" _ccache_path_list "$ENV{PATH}")
	set(_ccache_clean_path "")
	foreach(_dir IN LISTS _ccache_path_list)
		if(NOT _dir MATCHES "(^|[/\\])[^/\\]*ccache$")
			list(APPEND _ccache_clean_path "${_dir}")
		endif()
	endforeach()
	list(JOIN _ccache_clean_path "${_ccache_path_sep}" _ccache_sanitized_path)
	set(ENV{PATH} "${_ccache_sanitized_path}")
	unset(_ccache_path_sep)
	unset(_ccache_path_list)
	unset(_ccache_clean_path)
	unset(_ccache_sanitized_path)
endif()

# Project name and version
project(${PROJECT_NAME} VERSION 1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMAKE_CXX_MODULE_STD ON was set before project(). It tells CMake to build
# the std module and propagate its BMI reference to every consumer target
# through the target's modmap file (-fmodule-file=std=...). Without it,
# consumers fail with "module 'std' not found".
set(CMAKE_CXX_MODULE_STD ON)

# ---- C++26 std-module support ----
# CMAKE_CXX_COMPILER_IMPORT_STD is populated by CMake's toolchain probe on the
# releases that gate `import std;`. CMake >= 4.5 enables the feature generally
# and may not populate the variable, so only check it on older CMake.
if(CMAKE_VERSION VERSION_LESS 4.5
   AND DEFINED CMAKE_CXX_COMPILER_IMPORT_STD
   AND NOT "26" IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
	message(FATAL_ERROR
		"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} does not advertise "
		"C++26 std-module support. This project requires `import std;` in C++26. "
		"Use Clang >= 18.1.2 with libc++ (recommended) or GCC >= 15 with libstdc++.")
endif()

# ---- libc++ availability (only valid after project() — compiler is known) ----
if(USE_LIBCPP)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		include(CheckCXXSourceCompiles)

		set(CMAKE_REQUIRED_FLAGS "-stdlib=libc++")

		check_cxx_source_compiles("
			#include <string>
			int main() {
				std::string s = \"hello\";
				return s.size();
			}
		" HAS_LIBCXX)

		unset(CMAKE_REQUIRED_FLAGS)

		if(HAS_LIBCXX)
			message(STATUS "libc++ detected and working — using it")
			# Linker flag is not automatically propagated from CMAKE_CXX_FLAGS.
			add_link_options(-stdlib=libc++)
		else()
			message(FATAL_ERROR
				"USE_LIBCPP=ON but libc++ is not available. Install libc++ and its "
				"module metadata (libc++.modules.json), or configure with "
				"-DUSE_LIBCPP=OFF to use libstdc++.")
		endif()
	else()
		message(FATAL_ERROR
			"USE_LIBCPP=ON requires Clang; the configured compiler is "
			"${CMAKE_CXX_COMPILER_ID}. Configure with -DUSE_LIBCPP=OFF or use "
			"-DCMAKE_CXX_COMPILER=clang++.")
	endif()
endif()

# ccache is never enabled implicitly: its cache key does not cover C++ module
# BMI/IFC contents, so it can serve objects compiled against a stale module
# interface (silent ODR corruption). An explicitly requested launcher is still
# honoured, but warned about below.

# C++ modules (esp. `import std;`) are NOT safe under ccache: ccache's cache
# key does not cover C++ module BMI/IFC contents, so it can serve objects
# compiled against a stale module interface — silent ODR corruption. Warn
# loudly if an explicitly requested launcher or the resolved compiler resolves
# to ccache. This checks both the launcher property AND the compiler path (some
# distros, e.g. Fedora, put ccache wrapper dirs such as /usr/lib64/ccache early
# in PATH, so a bare `clang++`/`g++` is already the ccache wrapper).
set(_ccache_in_use OFF)
if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
	set(_ccache_in_use ON)
endif()
get_filename_component(_cc_name "${CMAKE_CXX_COMPILER}" NAME)
file(REAL_PATH "${CMAKE_CXX_COMPILER}" _cc_real)
get_filename_component(_cc_real_name "${_cc_real}" NAME)
if(_cc_name MATCHES "ccache" OR _cc_real_name MATCHES "ccache")
	set(_ccache_in_use ON)
endif()
if(_ccache_in_use)
	message(WARNING
		"The C++ compiler resolves to a ccache wrapper (${CMAKE_CXX_COMPILER}). "
		"ccache is unsafe with C++ modules (stale module-object hazard). Prefer "
		"the 'libcxx' preset (which disables it) or point CMake at the real "
		"compiler, e.g. -DCMAKE_CXX_COMPILER=/usr/bin/clang++.")
endif()
unset(_ccache_in_use)
unset(_cc_name)
unset(_cc_real)
unset(_cc_real_name)

# ── Sanitizers (Debug builds) ──────────────────────────────────────────
# ASan + UBSan are applied to the library, the app and the tests through the
# `project_sanitizers` interface target. They are only active in the Debug
# configuration, so Release/RelWithDebInfo are unaffected.
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer in Debug mode" ON)
# MinGW's runtimes do not ship the sanitizer runtimes (there is no UBSan for
# MinGW, and the Windows ASan runtime is MSVC-only), so force it off for the
# Windows target rather than emitting flags the linker cannot satisfy.
if(ENABLE_SANITIZERS AND WIN32)
    message(STATUS "Sanitizers are not available for Windows/MinGW targets — forcing ENABLE_SANITIZERS=OFF")
    set(ENABLE_SANITIZERS OFF)
endif()
# The sanitizer runtimes are separate packages on some distros (and clang needs
# compiler-rt), so probe an actual link before enabling them rather than letting
# the first Debug link fail. The probe caches its result, so drop any memoized
# value first.
if(ENABLE_SANITIZERS)
    include(CheckCXXSourceCompiles)
    unset(HAVE_SANITIZERS CACHE)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT HAVE_SANITIZERS)
        message(WARNING
            "AddressSanitizer/UndefinedBehaviorSanitizer are not available for "
            "${CMAKE_CXX_COMPILER_ID} — forcing ENABLE_SANITIZERS=OFF. Install the "
            "sanitizer runtime (libasan/libubsan, or clang's compiler-rt) to enable it.")
        set(ENABLE_SANITIZERS OFF)
    endif()
endif()
if(ENABLE_SANITIZERS)
    add_library(project_sanitizers INTERFACE)
    target_compile_options(project_sanitizers INTERFACE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
    target_link_options(project_sanitizers INTERFACE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
endif()

# ── Linker selection: use LLD when the host has it ─────────────────────
# LLD is optional, but when present it unlocks two size knobs the default GNU
# ld cannot offer: zstd-compressed debug sections and identical-code folding
# (--icf). The check is a real link test, so a host without lld simply keeps
# its default linker. The probe caches its result, so drop any memoized value
# first: installing lld after the first configure must be picked up by the next
# configure without deleting the build directory by hand.
include(CheckLinkerFlag)
unset(HAVE_LLD CACHE)
check_linker_flag(CXX "-fuse-ld=lld" HAVE_LLD)
if(HAVE_LLD)
    message(STATUS "lld found — linking with -fuse-ld=lld")
    add_link_options(-fuse-ld=lld)
else()
    message(STATUS "lld not found — using the default linker")
endif()

# Sections optimization for dead-code stripping (gc-sections)
add_library(project_sections INTERFACE)
target_compile_options(project_sections INTERFACE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffunction-sections>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fdata-sections>
)
target_link_options(project_sections INTERFACE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wl,--gc-sections>
)

# ── Size knobs that cost nothing in debuggability ──────────────────────
# Debug info is 60-85% of the bytes in Debug/RelWithDebInfo binaries, so
# compressing it is the single largest win available on ELF: gdb, lldb and perf
# decompress SHF_COMPRESSED sections transparently. zstd needs lld; a GNU ld
# without zstd falls back to zlib. The Windows target cannot compress its DWARF
# (lld's COFF driver rejects --compress-debug-sections), so it gets split DWARF
# instead: the detailed DIEs move into per-object .dwo files and the executable
# keeps skeleton units plus line tables, so gdb fidelity is unchanged as long
# as the build tree survives.
#
# --icf=safe is Release-only: it consults the .llvm_addrsig address-significance
# tables clang emits, so it never merges a function whose address is observable.
# It is kept out of Debug/RelWithDebInfo because a folded function shares one
# address with its twin, which makes breakpoints and backtraces ambiguous.
add_library(project_size INTERFACE)
if(WIN32)
    target_compile_options(project_size INTERFACE
        $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>>:-gsplit-dwarf>)
else()
    if(HAVE_LLD)
        target_link_options(project_size INTERFACE
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wl,--compress-debug-sections=zstd>
        )
        target_link_options(project_size INTERFACE
            $<$<CONFIG:Release>:-Wl,--icf=safe>
        )
    else()
        target_link_options(project_size INTERFACE
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wl,--compress-debug-sections=zlib>
        )
    endif()
endif()

# Enable CTest at the top level so `ctest` discovers tests
include(CTest)

# Include custom CMake modules path (for other helpers)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(DEFAULT_LOGGING_INTERFACE_DIR "${CMAKE_SOURCE_DIR}/external/cppLoggingInterface")
set(LOGIFACE_PATH "" CACHE PATH "Optional path to the cppLoggingInterface repo")

if(LOGIFACE_PATH)
	set(LOGGING_INTERFACE_DIR "${LOGIFACE_PATH}")
else()
	set(LOGGING_INTERFACE_DIR "${DEFAULT_LOGGING_INTERFACE_DIR}")
endif()

# Build the logging interface as a C++ module (`import logiface;` /
# `import logiface.ConsoleLogger;`). The macro-only `logging_macros.hpp` header
# has no `#include` directives, so it is safe before `import std;` in a module
# unit's global module fragment and in ordinary test translation units.
set(LOGIFACE_BUILD_MODULE ON CACHE BOOL "" FORCE)
# The logging module itself also uses `import std;` instead of standard headers.
set(LOGIFACE_USE_STD_MODULE ON CACHE BOOL "" FORCE)
# GCC's `.gcm` files are dialect-strict, so the logging module must be built
# with exactly the consumer's standard.
set(LOGIFACE_CXX_STANDARD 26 CACHE STRING "" FORCE)

if(EXISTS "${LOGGING_INTERFACE_DIR}/CMakeLists.txt")
	add_subdirectory("${LOGGING_INTERFACE_DIR}" cppLoggingInterface)
else()
	message(FATAL_ERROR "Missing logging interface at ${LOGGING_INTERFACE_DIR}; provide the repo there or set LOGIFACE_PATH to another directory.")
endif()

option(ENABLE_LOGGING "Enable header-only logging interface" ON)

option(CLANG_TIDY_ENABLED "Enable Clang-Tidy static analysis" ON)
# disable Clang-Tidy if not using Clang or GCC
if(CLANG_TIDY_ENABLED AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(CLANG_TIDY_ENABLED OFF)
endif()
if(CLANG_TIDY_ENABLED)
	# Enable Clang-Tidy if available
	include(ConfigureClangTidy)
endif()

# ── Windows runtime DLL deployment ─────────────────────────────────────
# Directories that deploy_runtime_dlls() scans to place the shared runtime and
# dependency DLLs next to each built executable:
#   * vcpkg dependencies (dynamic on the MinGW triplet), Debug and Release kept
#     in separate trees by vcpkg;
#   * the llvm-mingw runtimes (libc++.dll, libunwind.dll, libwinpthread-1.dll).
if(WIN32)
    set(_tpl_vcpkg_runtime_dir "$<$<CONFIG:Debug>:${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/bin>$<$<NOT:$<CONFIG:Debug>>:${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin>")
    set(TEMPLATE_RUNTIME_DLL_DIRS "${_tpl_vcpkg_runtime_dir}")
    if(DEFINED LLVM_MINGW_ROOT AND LLVM_MINGW_ROOT)
        list(APPEND TEMPLATE_RUNTIME_DLL_DIRS
            "${LLVM_MINGW_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-w64-mingw32/bin")
    endif()
    unset(_tpl_vcpkg_runtime_dir)
endif()

# Copy the executable's transitive runtime DLLs next to it (Windows/MinGW only).
# The candidate directories must be escaped before going on a command line: a
# raw ';'-separated list is written into the generated shell command verbatim,
# so /bin/sh splits it and runs everything after the first semicolon as a
# separate command. Escaping the separators makes the shell pass the script a
# single argument that still reads as a list inside CMake.
function(deploy_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()
    string(REPLACE ";" "\\;" _dirs "${TEMPLATE_RUNTIME_DLL_DIRS}")
    add_custom_command(
        TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DDLL_DIRS=${_dirs}"
            "-DEXE=$<TARGET_FILE:${target}>"
            "-DOBJDUMP=${CMAKE_OBJDUMP}"
            "-DSTRIP=$<IF:$<CONFIG:Release>,${CMAKE_STRIP},>"
            "-DDEST=$<TARGET_FILE_DIR:${target}>"
            -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake"
        COMMENT "Deploying Windows runtime DLLs for ${target}"
    )
endfunction()

# Add subdirectories for source and tests
add_subdirectory(src)

if(BUILD_TESTING)
	add_subdirectory(tests)
endif()
