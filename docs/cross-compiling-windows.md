# Cross-compiling for Windows (Linux → Windows x86_64)

This template can be cross-compiled from a Linux x86_64 host to Windows x86_64
using [llvm-mingw](https://github.com/mstorsjo/llvm-mingw): a self-contained
Clang/LLD toolchain that ships the mingw-w64 sysroot, compiler-rt, libunwind and
libc++ — **including the libc++ standard-library module metadata**, which is what
makes `import std;` work on the Windows target.

## 1. Prerequisites

- Linux x86_64 build host
- CMake ≥ 4.2 (4.2 introduced `CMAKE_CXX_STDLIB_MODULES_JSON`, which the cross
  toolchain uses to point CMake at the *target* libc++ module metadata; the
  CMake bundled with recent CLion releases works)
- Ninja
- vcpkg, with `VCPKG_ROOT` set
- *optional*: `wine`, to run the resulting executables

## 2. Download llvm-mingw into the project

The toolchain is expected **inside this project** at `toolchains/`. That
directory is listed in `.gitignore`, so the toolchain is never committed and
nothing outside the checkout is required.

```bash
cd /path/to/modern-cmake-app-template

mkdir -p toolchains
curl -L -o toolchains/llvm-mingw.tar.xz \
  https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64.tar.xz

# verify the download
echo "2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092  toolchains/llvm-mingw.tar.xz" | sha256sum -c -

tar -xf toolchains/llvm-mingw.tar.xz -C toolchains
rm toolchains/llvm-mingw.tar.xz
```

After extracting you have:

```
toolchains/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/
```

Use the **ucrt** variant (the msvcrt variant is only for very old Windows).
`cmake/toolchains/llvm-mingw.cmake` auto-detects `toolchains/llvm-mingw*`, so no
environment variables are needed. To keep the toolchain somewhere else, pass
`-DLLVM_MINGW_ROOT=/path/to/llvm-mingw` (or set the environment variable of the
same name).

Sanity check — the second file is the one that enables `import std;`:

```bash
toolchains/llvm-mingw-*/bin/x86_64-w64-mingw32-clang++ --version
ls toolchains/llvm-mingw-*/x86_64-w64-mingw32/lib/libc++.modules.json
```

## 3. Configure

```bash
export VCPKG_ROOT=/path/to/vcpkg   # only needed if not already set
cmake --preset windows
```

The `windows` preset selects the `x64-mingw-libcxx` vcpkg triplet, enables
libc++, and turns off the options that are not available for a MinGW target
(sanitizers, tests, clang-tidy).

## 4. Build

```bash
cmake --build --preset windows-release        # or windows-debug / windows-relwithdebinfo
```

The executable lands under `build-windows/bin/<Config>/` together with the
runtime DLLs that `deploy_runtime_dlls()` copies next to it (see
`cmake/CopyRuntimeDlls.cmake`).

## 5. Run (optional, via wine)

```bash
wine build-windows/bin/Release/ModernCMakeApp.exe
```

## How this differs from the native Linux build

- **Sanitizers are unavailable** on MinGW and are forced off, so the
  `project_sanitizers` interface target is not created.
- **Debug info is split rather than compressed.** lld's COFF driver rejects
  `--compress-debug-sections`, so Debug/RelWithDebInfo use `-gsplit-dwarf`; the
  build tree must survive for symbol-level debugging.
- **Dependencies are dynamic.** The MinGW triplet links shared libraries, so the
  executable needs the runtime DLLs beside it — which is what the POST_BUILD
  deployment step provides.

## Troubleshooting

- **"Stale CMake build tree"** — the toolchain refuses to configure a build
  directory that was created before the cross toolchain existed (CMake keeps a
  cached `CMakeSystem.cmake` reporting a native system). Delete the directory
  and configure again:
  ```bash
  rm -rf build-windows && cmake --preset windows
  ```
- **`wine: Library libc++.dll (or libunwind.dll) not found`** — run the
  executable from its output directory (the build copies the llvm-mingw
  runtimes there), e.g. `wine build-windows/bin/Release/ModernCMakeApp.exe`.
- **Updating llvm-mingw** — download a newer release into `toolchains/` (the
  newest `toolchains/llvm-mingw*` directory is picked up automatically) and
  delete `build-windows`.

## Related files

- `cmake/toolchains/llvm-mingw.cmake` — the cross toolchain file
- `cmake/triplets/x64-mingw-libcxx.cmake` — the vcpkg triplet
- `cmake/CopyRuntimeDlls.cmake` — runtime DLL deployment
- `CMakePresets.json` — the `windows` configure preset and `windows-*` build presets
