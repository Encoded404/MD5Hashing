cmake_minimum_required(VERSION 3.30)

# Enable CMake's std-module support (`import std;`).
# import std; is still experimental in CMake, so older versions require a
# feature guid. CMake >= 4.3 no longer needs one.
if( NOT CMAKE_EXPERIMENTAL_CXX_IMPORT_STD )
	if( ${CMAKE_VERSION} VERSION_GREATER_EQUAL 4.3 )
		set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 451f2fe2-a8a2-47c3-bc32-94786d8fc91b)
	elseif( ${CMAKE_VERSION} VERSION_GREATER_EQUAL 3.30 )
		set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
	endif ()
endif ()

set(PROJECT_NAME "ModernCMakeAppTemplate")
set(LIBRARY_TARGET "ModernCMakeAppInternalLib")
set(APP_TARGET "ModernCMakeApp")

# if this is not set by the user cmake will decide
set(OUTPUT_PATH "${CMAKE_BINARY_DIR}/bin")

# `USE_CXX_MODULES`: toggle for C++23 standard-library modules (`import std;`).
#   ON  -> C++23 + CMAKE_CXX_MODULE_STD, and the example code uses `import std;`.
#         Requires a Clang or GCC compiler (MSVC has no C++23 std module).
#   OFF -> C++20 + classic `#include <...>` headers. Useful when the toolchain
#         or stdlib cannot reliably build the std module (e.g. Clang +
#         libstdc++, which currently crashes on <memory> in module contexts).
option(USE_CXX_MODULES "Use C++23 standard-library modules (import std;)" ON)

# `USE_LIBCPP`: build against libc++ instead of libstdc++ (Clang only).
#   `import std;` with Clang + libstdc++ currently rides on GCC's bits/std.cc
#   and hits real compiler crashes (e.g. <memory>), so the std module is
#   practically always paired with libc++, which ships a proper module map
#   (libc++.modules.json).
#   NOTE: when ON, the vcpkg dependency tree must also be built against libc++,
#   otherwise ABI mismatches occur. That's what the `libcxx` preset does via the
#   overlay triplet cmake/triplets/x64-linux-libcxx.cmake.
option(USE_LIBCPP "Use libc++ instead of libstdc++ (Clang only)" OFF)

set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "Use standard C++" FORCE)

# ---- IMPORTANT: stdlib flags must be set BEFORE project() ----
#
# CMake's built-in std-module detection (the __cmake_cxx23 target) runs during
# project(), where it discovers the compiler frontend variant and the
# <source>/<bits>/std.cc file to use for `import std;`.
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

# The C++ standard drives everything else in this template: C++23 enables
# `import std;`, C++20 keeps the classic header-based path.
if(USE_CXX_MODULES)
	set(CMAKE_CXX_STANDARD 23)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	# Set before project() so CMake's std-module detection is active while the
	# compiler (and its std module files) are being probed.
	set(CMAKE_CXX_MODULE_STD ON)
else()
	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

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

if(USE_CXX_MODULES)
	# CMAKE_CXX_MODULE_STD ON tells CMake to build the <std> module (the
	# auto-created __cmake_cxx23 target) and propagate the reference down to
	# every consumer target through its modmap file
	# (-fmodule-file=std=.../std.pcm). Without the ON flag the consumers fail
	# with "module 'std' not found".
	set(CMAKE_CXX_MODULE_STD ON)

	# ---- Compiler gate ----
	# The template is not designed around MSVC, and `import std;` does not exist
	# there. Rather than hard-failing, warn and fall back to the header path so
	# the project still configures and builds.
	set(USE_STD_MODULE ${USE_CXX_MODULES})
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
		message(WARNING
			"USE_CXX_MODULES=ON but the compiler (${CMAKE_CXX_COMPILER_ID}) has no C++23 "
			"std module support. Falling back to classic #include headers.")
		set(USE_STD_MODULE OFF)
	endif()
else()
	set(USE_STD_MODULE OFF)
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

		if (HAS_LIBCXX)
			message(STATUS "libc++ detected and working — using it")

			# Force -stdlib=libc++ onto the auto-generated std-module targets so
			# the module BMI is compiled against the same libc++ headers that the
			# rest of the project uses.
			if(TARGET __cmake_cxx23)
				set_property(TARGET __cmake_cxx23 APPEND PROPERTY COMPILE_OPTIONS "-stdlib=libc++")
			endif()
			if(TARGET __cmake_cxx26)
				set_property(TARGET __cmake_cxx26 APPEND PROPERTY COMPILE_OPTIONS "-stdlib=libc++")
			endif()

			# Linker flag is not automatically propagated from CMAKE_CXX_FLAGS.
			add_link_options(-stdlib=libc++)
		else()
			message(WARNING "USE_LIBCPP=ON but libc++ is NOT available — falling back to libstdc++")
			string(REPLACE "-stdlib=libc++" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
			if(USE_STD_MODULE)
				# Clang + libstdc++ cannot reliably build the std module (the
				# <memory>/bits/std.cc issue above) — fall back to headers too.
				message(WARNING "Disabling std-module support because libc++ is unavailable.")
				set(USE_STD_MODULE OFF)
			endif()
		endif()
	else()
		message(WARNING "USE_LIBCPP=ON but the compiler is not Clang — ignoring")
		string(REPLACE "-stdlib=libc++" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	endif()
endif()

# Tell the source files whether the C++23 std module is actually in use.
# The example code branches between `import std;` and `#include <...>` based on
# this define (see src/lib1/lib_example.cpp and src/app/main.cpp).
if(USE_STD_MODULE)
	add_compile_definitions(USE_STD_MODULE=1)
endif()

# enable ccache (unsafe with modules — see warning below)
find_program(CCACHE_PROGRAM ccache)

# ccache is only auto-enabled on the header-only (non-module) path. With C++
# modules it is not safe (stale module-object hazard), so we never enable it
# implicitly there — an explicitly requested launcher is still honoured and
# merely warned about further down.
if(CCACHE_PROGRAM AND NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER AND NOT USE_STD_MODULE)
	#set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM}) # we dont use the c compiler rn
	set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

# C++ modules (esp. `import std;`) are NOT safe under ccache: ccache's cache
# key does not cover C++ module BMI/IFC contents, so it can serve objects
# compiled against a stale module interface — silent ODR corruption. We keep
# ccache usable (the header-based path is fine) but warn loudly when modules
# are enabled together with ccache so it is not a silent foot-gun. This checks
# both the launcher property AND the resolved compiler path (some distros, e.g.
# Fedora, put ccache wrapper dirs such as /usr/lib64/ccache early in PATH, so a
# bare `clang++`/`g++` is already the ccache wrapper).
if(USE_STD_MODULE)
	set(_ccache_in_use OFF)
	if(CCACHE_PROGRAM AND CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
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
endif()

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
# `import logiface.ConsoleLogger;`) when modules are enabled, and as plain
# headers otherwise. The macro-only `logging_macros.hpp` header is safe to drop
# into a module's global module fragment next to `import std;` (no std includes,
# avoids the `<memory>` clash / Clang bug LLVM #138558).
set(LOGIFACE_BUILD_MODULE ${USE_CXX_MODULES} CACHE BOOL "" FORCE)
# Keep the logging module itself on `#include` headers instead of `import std;`
# for the same reason.
set(LOGIFACE_USE_STD_MODULE OFF CACHE BOOL "" FORCE)

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
