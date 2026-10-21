# Windows x86_64 target built from Linux with llvm-mingw (Clang + libc++).
#
# - VCPKG_CMAKE_SYSTEM_NAME=MinGW makes vcpkg treat the target as Windows
#   (VCPKG_TARGET_IS_WINDOWS / VCPKG_TARGET_IS_MINGW), which is what port
#   logic and `supports` platform expressions expect for a MinGW toolchain.
# - The chainloaded toolchain (cmake/toolchains/llvm-mingw.cmake) replaces
#   vcpkg's default scripts/toolchains/mingw.cmake and points the compilers at
#   llvm-mingw's per-target Clang wrappers.
#
# Dynamic linkage is the norm on Windows and is what the runtime-DLL deployment
# step (cmake/CopyRuntimeDlls.cmake) assumes when it collects the executable's
# import closure.
#
# Select with: -DVCPKG_TARGET_TRIPLET=x64-mingw-libcxx
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME MinGW)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/llvm-mingw.cmake")

# Newer Clang (>= 21) diagnoses `-ffp-contract=off` after `-ffp-model=precise`
# with -Woverriding-option. Some ports pass both and build with -Werror, which
# turns that diagnostic into a hard error. Disable it for all ports (the same
# flag is already used by the native clang-libcxx toolchain). A fully-disabling
# -Wno-<diag> is not resurrected by a later -Werror.
#
# -ffunction-sections/-fdata-sections give every port per-function/per-object
# data granularity. On this triplet the dependencies are DLLs, so those flags
# only pay off together with --gc-sections at the ports' own link step, which
# is why it is added to VCPKG_LINKER_FLAGS below (a consumer-side linker flag
# cannot reach inside an already-linked DLL).
set(VCPKG_CXX_FLAGS "-Wno-overriding-option -ffunction-sections -fdata-sections")
set(VCPKG_C_FLAGS "-Wno-overriding-option -ffunction-sections -fdata-sections")

# Discard unreachable code inside the dependency DLLs. Safe for MinGW DLLs:
# lld treats every export as a collection root regardless of how it is
# declared — __declspec(dllexport), a .def file, or --export-all-symbols — so
# the exported ABI cannot be collected.
set(VCPKG_LINKER_FLAGS "${VCPKG_LINKER_FLAGS} -Wl,--gc-sections")
