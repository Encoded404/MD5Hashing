set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Build every vcpkg dependency against libc++ (clang), so that the whole
# graph is ABI-compatible with a `std` module compiled against libc++.
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/clang-libcxx.cmake")
