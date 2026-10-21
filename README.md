# modern-cmake-template

Modern CMake starter that keeps the library as the main product, adds a thin CLI to exercise it, and wires in GoogleTest via vcpkg. Comes with C++23 **standard-library modules** (`import std;`) support and an optional libc++/Clang configuration. Clang-Tidy can be enabled when available.

## Layout

```
modern-cmake-app-template/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
│   ├── ConfigureClangTidy.cmake
│   ├── CopyRuntimeDlls.cmake               # Windows runtime-DLL deployment
│   ├── toolchains/
│   │   ├── clang-libcxx.cmake              # vcpkg chainload toolchain for libc++
│   │   └── llvm-mingw.cmake                # Linux -> Windows cross toolchain
│   └── triplets/
│       ├── x64-linux-libcxx.cmake          # vcpkg overlay triplet (libc++)
│       └── x64-mingw-libcxx.cmake          # vcpkg overlay triplet (Windows/MinGW)
├── docs/
│   └── cross-compiling-windows.md
├── src/
│   ├── CMakeLists.txt
│   ├── app/
│   │   ├── CMakeLists.txt
│   │   └── main.cpp                        # import std; + import logiface; + import lib_example;
│   └── lib1/
│       ├── CMakeLists.txt
│       ├── lib_example.cpp                 # module lib_example; (definition)
│       └── lib_example.cppm                # export module lib_example; (interface)
├── tests/
│   ├── CMakeLists.txt
│   ├── example_tests.cpp
│   └── test_logging.hpp
├── vcpkg.json
└── vcpkg-configuration.json
```

## Prerequisites

- CMake ≥ 3.30
- A Clang or GCC compiler (required for C++ modules / `import std;`)
- vcpkg available and `VCPKG_ROOT` set (the presets expect it)
- `cppLoggingInterface` submodule at `5cf75d9` or newer (run `git submodule update --init`)
- ccache-aware distros (Fedora): PATH ccache shims are stripped automatically, see "ccache is the real hazard" below

## VCPKG installation
you can follow the guide [here](https://github.com/microsoft/vcpkg) for installation

## Clone

```bash
git clone <repository-url>
cd modern-cmake-app-template
git submodule update --init            # fetches external/cppLoggingInterface
```

## Configure

Use the provided Ninja multi-config preset:

```bash
cmake --preset default -S . -B build
```

Options:
- `-DBUILD_TESTING=OFF` to skip tests (default: ON via preset)
- `-DENABLE_LOGGING=OFF` to compile out logging macros (default: ON)
- `-DCLANG_TIDY_ENABLED=OFF` to skip clang-tidy configuration
- `-DUSE_CXX_MODULES=OFF` to disable the C++23 std module and build as C++20 with classic headers
- `-DUSE_LIBCPP=ON` to build against libc++ instead of libstdc++ (Clang only)
- `-DENABLE_SANITIZERS=OFF` to skip the Debug ASan/UBSan build (default: ON, and forced OFF on Windows/MinGW)

If you prefer a manual invoke, or vcpkg lives elsewhere, specify the toolchain and cache toggles directly:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DBUILD_TESTING=ON
```

## Build

```bash
cmake --build build --config Debug   # or Release/RelWithDebInfo
```

## Tests

Tests are controlled by `BUILD_TESTING` (default ON in the preset). Disable with `-DBUILD_TESTING=OFF` if you only want the app:

```bash
ctest --test-dir build -C Debug
```

## C++ modules & the std module

This template uses CMake's std-module support (`CMAKE_CXX_MODULE_STD`) so the example code can `import std;`.

- **`USE_CXX_MODULES=ON` (default)** — C++23 + `import std;`. The source branches on the `USE_STD_MODULE` compile definition: module builds use `import std;`, everything else falls back to `#include <...>`. User modules (`export module lib_example;`) are always built via `FILE_SET CXX_MODULES`; applications/tests consume them with `import lib_example;`.
- **`USE_CXX_MODULES=OFF`** — C++20 + classic headers (the std module path only). Useful on toolchains where `import std;` is unreliable. `--preset modules-off` gives this directly.

### Logging: the macros + module pattern

`import std;` cannot be mixed with textual `#include <string>`/`<memory>` etc. in the same TU — the standard declarations collide with the module's. Headers that pull in the standard library (like a classic `#include <logging/logging.hpp>`) therefore break in module TUs. The (newer) logging interface solves this by splitting into:

- **`<logging/logging_macros.hpp>`** — a *macro-only* header with **no `#include` directives**, so it is safe to place in the Global Module Fragment of a module unit next to `import std;`. The `Logiface::` types it references come from `import logiface;`.
- **`logiface` / `logiface.ConsoleLogger` C++ modules** (`LOGIFACE_BUILD_MODULE`, enabled whenever `USE_CXX_MODULES` is on). The module itself stays on `#include` headers (`LOGIFACE_USE_STD_MODULE=OFF`) because building *it* with `import std;` trips Clang bug LLVM #138558.

So in module mode the sources do:

```cpp
module;                                  // global module fragment
#include <logging/logging_macros.hpp>    // macro-only, safe
module lib_example;
import std;
import logiface;                         // Logiface types
```

and in classic mode they fall back to `#include <logging/logging.hpp>` + `#include <logging/ConsoleLogger.hpp>`. Note the newer logging interface uses a raw-pointer API: `Logiface::ConsoleLogger app_logger;` + `Logiface::SetLogger(&app_logger);`.

> The logging submodule must be at `5cf75d9` or later for this (`logging_macros.hpp` + module support).

### ccache is the real hazard

C++ modules (especially `import std;`) are **unsafe under ccache**: ccache's cache key does not cover module BMI/IFC contents, so after a module interface changes it can serve objects built against a stale interface — often surfacing as *bizarre* compile errors (redefinition of standard-library templates, "requires clause differs", crashes in `<memory>`/`bits/std.cc`). Critically, `CCACHE_DISABLE=1` is **not enough** — ccache runs at build time, and if the compiler *is* a ccache wrapper it still interferes.

The template therefore (like VulcanEngineV5):
1. strips ccache wrapper dirs (e.g. `/usr/lib64/ccache`) from `PATH` **before `project()`**, so CMake detects and uses the **real** compiler binary, and
2. warns loudly if modules are on and the compiler/launcher still resolves to ccache.

This is why the `default`/`libcxx` presets behave on distros that shim every `clang++`/`g++`/`c++` through ccache (e.g. Fedora). If you explicitly want ccache, keep it to the non-module path (`USE_CXX_MODULES=OFF`).

### The golden rule: never mix textual std headers with `import std;`

The crash you'll hit is **not** the compiler or the stdlib — it's having a *standard-library header included textually in the same translation unit (or same module's global module fragment) that also does `import std;`*:

```cpp
import std;
#include <memory>     // ✗ declarations collide with module std
```

The textual include gets attached to the module's global fragment (`std.<global>`), re-declaring entities the module already owns → "requires clause differs in template redeclaration", "redefinition of `__promote`/concepts", "conflicting imported declaration", `cannot befriend target of using declaration`, etc.

Whether mixing is tolerated is toolchain-specific (measured on GCC 15.2 / Clang 21 / Fedora, with a std header placed in a module unit's global module fragment):

| mixing std headers + `import std;` | result |
|---|---|
| clang + libc++ | ✅ works (headers in GMF, or include-before-import) |
| clang + libstdc++ | ❌ broken |
| gcc + libstdc++ | ❌ broken |

Even on clang + libc++ it's brittle (order/header-sensitive), so the template's **portable rule** is: keep `#include`s of any standard-library header **out of any TU that imports `std`**. The `logging_macros.hpp` + `import logiface;` pattern above is exactly this: `logging_macros.hpp` has no `#include`s, so it can live next to `import std;`.

For third-party header-only libraries that don't support the std module yet (which is most of them), the same recipe applies — they should preferably expose a macro-only header (like the logging lib does) plus a real C++ module. If that's not possible and you must include their headers, do so in a *separate* non-module TU (a `.cpp` that only uses `#include`s and never `import std;`), or in a module unit's global module fragment **and only rely on a toolchain verified to tolerate it (clang + libc++)**.

### Selecting the C++ standard library

Given the golden rule is respected, `import std;` builds on **gcc + libstdc++**, **clang + libstdc++**, and **clang + libc++** (verified here). Toolchain defaults:

- `default` — system compiler + libstdc++ (works with gcc or clang).
- `libcxx` — Clang + libc++ via the `x64-linux-libcxx` overlay triplet. libc++ ships a proper module map (`libc++.modules.json`) and its std module is the most robust; libstdc++'s std module is a `<bits>/std.cc`-based approximation that is more likely to break with future headers. libc++ is the recommended long-term default for `import std;`.

Because a `std` BMI compiled against libc++ needs an ABI-compatible graph, every vcpkg dependency must also be built against libc++ — hence the `x64-linux-libcxx` overlay triplet (`cmake/triplets`, registered in `vcpkg-configuration.json`), used by the `libcxx` preset, which forces Clang, `USE_LIBCPP=ON`, and a one-time rebuild of the vcpkg deps. On Fedora the `/etc/clang/x86_64-redhat-linux-gnu-clang++.cfg` file can make Clang find libstdc++'s `bits/std.cc` even under `-stdlib=libc++`; the top-level CMakeLists handles this by pushing `-stdlib=libc++` into the initial flags before `project()`.

```bash
cmake --preset libcxx
cmake --build --preset libcxx-debug
ctest --preset libcxx-debug
```

## Cross-compiling for Windows

Windows x86_64 binaries can be built from a Linux host with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang + libc++, including the
standard-library module, so `import std;` works on the target). Keep the release
extracted in `toolchains/` (gitignored), then:

```bash
cmake --preset windows
cmake --build --preset windows-release
```

The `windows` preset selects the `x64-mingw-libcxx` overlay triplet, enables libc++,
and turns off sanitizers/tests/clang-tidy. The executable lands under
`build-windows/bin/<Config>/` with its runtime DLLs deployed next to it. Cross builds
need **CMake ≥ 4.2** for `import std;`. See
**[docs/cross-compiling-windows.md](docs/cross-compiling-windows.md)** for the full
guide and troubleshooting.

## Binary size & optimization

Three interface targets carry the size/optimization flags, linked onto the library,
the app and the tests:

- **`project_sections`** — `-ffunction-sections -fdata-sections` plus `-Wl,--gc-sections`,
  so the linker can drop unreachable code instead of whole objects.
- **`project_size`** — compresses debug info (`--compress-debug-sections=zstd` with lld,
  `zlib` otherwise) and adds `--icf=safe` in Release only. On the Windows target, where
  lld's COFF driver rejects debug-section compression, it uses `-gsplit-dwarf` instead.
  Debug/RelWithDebInfo stay fully debuggable (gdb/lldb decompress transparently).
- **`project_sanitizers`** — `-fsanitize=address,undefined` in Debug only
  (`-DENABLE_SANITIZERS=OFF` to disable; forced off on Windows/MinGW).

The build also auto-detects LLD via a real link test (`-fuse-ld=lld`) and, in Release,
links the app with `-Wl,--strip-all` so no symbol table is emitted.

## Clang-Tidy

Set `-DCLANG_TIDY_ENABLED=ON` (default) to enable static analysis when `clang-tidy` is found and a `.clang-tidy` file is present at the project root. Set it to `OFF` to skip configuring Clang-Tidy even if available.
