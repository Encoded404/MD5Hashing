# modern-cmake-template

Modern CMake starter that keeps the library as the main product, adds a thin CLI to exercise it, and wires in GoogleTest via vcpkg. Requires **C++26** and uses the **standard-library module** (`import std;`) throughout first-party code instead of standard headers. There is no header-based fallback. Clang-Tidy can be enabled when available.

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
│   │   └── main.cpp                        # import std; + import logiface; + import hashlib;
│   └── HashLib/
│       ├── CMakeLists.txt
│       ├── hashlib.cpp                     # module hashlib; (definition)
│       └── hashlib.cppm                    # export module hashlib; (interface)
├── tests/
│   ├── CMakeLists.txt
│   ├── hashlib_tests.cpp                   # gtest headers first, then import std;
│   └── test_logging.cppm                   # gtest in the global module fragment
├── vcpkg.json
└── vcpkg-configuration.json
```

## HashLib API

The library takes raw bytes and returns raw bytes, so it works for both text and
binary input:

```cpp
import std;
import hashlib;

const std::string_view text = "hello";
const hashlib::Digest digest =
    hashlib::md5(std::as_bytes(std::span{text.data(), text.size()}));  // std::array<std::byte, 16>
```

`hashlib::Digest` is the raw 16-byte result; format it as hex (or base64) at the
call site if a printable representation is needed.

The MD5 core is intentionally not implemented yet; the placeholder throws
`std::logic_error` until the algorithm lands.

## Prerequisites

- CMake ≥ 3.30 (3.30-4.2 share one activation value; 4.3 and 4.4 have their own; ≥ 4.5 needs no gate. 4.3+ recommended)
- Clang ≥ 18.1.2 with libc++ (recommended), or GCC ≥ 15 with libstdc++
- Ninja ≥ 1.11
- vcpkg available and `VCPKG_ROOT` set (the presets expect it)
- `cppLoggingInterface` submodule at `baa3a9e` or newer (run `git submodule update --init`)
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
- `-DUSE_LIBCPP=ON` to build against libc++ instead of libstdc++ (Clang only; configuration fails if libc++ is unavailable)
- `-DENABLE_SANITIZERS=OFF` to skip the Debug ASan/UBSan build (default: ON, and forced OFF on Windows/MinGW)

C++26 and `import std;` are mandatory; there is no header-based configuration to fall back to.

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

This template requires C++26 and uses CMake's std-module support
(`CMAKE_CXX_MODULE_STD ON`). First-party translation units that need the
standard library use `import std;` instead of standard headers; there is no
`USE_CXX_MODULES` toggle and no header-based fallback. User modules (`export
module hashlib;`) are built via `FILE_SET CXX_MODULES`; the app and tests
consume them with `import hashlib;`.

### Logging: the macros + module pattern

The logging interface uses a macro-only header plus C++ modules:

- **`<logging/logging_macros.hpp>`** — a *macro-only* header with **no
  `#include` directives**, so it is safe to place in the global module fragment
  of a module unit, and before `import std;` in a normal TU. The `Logiface::`
  types it references come from `import logiface;`.
- **`logiface` / `logiface.ConsoleLogger` modules** — built with
  `LOGIFACE_USE_STD_MODULE=ON`, so the logging module itself also uses
  `import std;`.

Module implementation units look like this:

```cpp
module;                                  // global module fragment
#include <logging/logging_macros.hpp>    // macro-only, safe
module hashlib;
import std;
import logiface;                         // Logiface types
```

Note the logging interface uses a raw-pointer API: `Logiface::ConsoleLogger
app_logger;` + `Logiface::SetLogger(&app_logger);`.

### GTest and `import std;`

GTest is preprocessor-based and its headers pull in standard headers. That does
not prevent using the std module, but it fixes the order:

```cpp
#include <gtest/gtest.h>                  // must come first
#include <logging/logging_macros.hpp>     // macro-only, also safe first

import std;                               // imports come after textual headers
import test_logging;
import hashlib;
```

Never write `import std;` and then include a gtest (or other std-pulling)
header: the textual declarations would collide with the declarations owned by
the `std` module. This is the same pattern used by VulkanEngineV5 across its
test suite. Keep GTest's include directory on `-isystem` (the `GTest::gtest`
target does this) so third-party warnings do not trip `-Werror`.

A test-support module puts the gtest include in its global module fragment,
before `export module`:

```cpp
module;
#include <gtest/gtest.h>

export module test_logging;

import std;
import logiface;
import logiface.ConsoleLogger;
```

> The logging submodule must be at `baa3a9e` or later for the configurable C++
> standard and `LOGIFACE_USE_STD_MODULE` support.

### ccache is the real hazard

C++ modules (especially `import std;`) are **unsafe under ccache**: ccache's cache key does not cover module BMI/IFC contents, so after a module interface changes it can serve objects built against a stale interface — often surfacing as *bizarre* compile errors (redefinition of standard-library templates, "requires clause differs", crashes in `<memory>`/`bits/std.cc`). Critically, `CCACHE_DISABLE=1` is **not enough** — ccache runs at build time, and if the compiler *is* a ccache wrapper it still interferes.

The template therefore (like VulkanEngineV5):
1. strips ccache wrapper dirs (e.g. `/usr/lib64/ccache`) from `PATH` **before `project()`**, so CMake detects and uses the **real** compiler binary, and
2. warns loudly if the compiler/launcher still resolves to ccache.

ccache is never enabled implicitly, because every configuration now uses modules. The `default`/`libcxx` presets behave on distros that shim every `clang++`/`g++`/`c++` through ccache (e.g. Fedora).

### Include order with textual standard headers

The collision is caused by including a standard-library header **after**
`import std;`, not by textual headers in general:

```cpp
import std;
#include <memory>     // ✗ declarations collide with module std
```

The include after the import attaches its declarations to the module purview
while the `std` module already owns them, producing errors such as "requires
clause differs in template redeclaration", "redefinition of
`__promote`/concepts", or "conflicting imported declaration".

The reverse order is the supported pattern and is what the logging and GTest
integrations use:

```cpp
#include <gtest/gtest.h>                  // headers first
#include <logging/logging_macros.hpp>

import std;                               // imports second
import logiface;
```

This include-before-import pattern is used by VulkanEngineV5 across its test
suite (Clang + libc++), and works under C++26. Keep third-party include
directories on `-isystem` so their warnings do not break `-Werror`.

For third-party header-only libraries that do not support the std module yet,
the same recipe applies: include their headers before `import std;`, or — when
they expose a macro-only header and a real C++ module, as the logging interface
does — use the module form.

### Selecting the C++ standard library

With the include-order rule respected, `import std;` builds on **gcc + libstdc++**, **clang + libstdc++**, and **clang + libc++** under C++26 (verified here). Toolchain defaults:

- `default` — system compiler + libstdc++ (works with gcc or clang).
- `libcxx` — Clang + libc++ via the `x64-linux-libcxx` overlay triplet. libc++ ships a proper module map (`libc++.modules.json`) and its std module is the most robust; libstdc++'s std module is a `<bits>/std.cc`-based approximation that is more likely to break with future headers. libc++ is the recommended long-term default for `import std;`. The GTest module-wrapper pattern is exercised with Clang + libc++ (VulkanEngineV5) and also compiles with GCC 15 + libstdc++.

Because a `std` BMI compiled against libc++ needs an ABI-compatible graph, every vcpkg dependency must also be built against libc++ — hence the `x64-linux-libcxx` overlay triplet (`cmake/triplets`, registered in `vcpkg-configuration.json`), used by the `libcxx` preset, which forces Clang, `USE_LIBCPP=ON`, and a one-time rebuild of the vcpkg deps. On Fedora the `/etc/clang/x86_64-redhat-linux-gnu-clang++.cfg` file can make Clang find libstdc++'s `bits/std.cc` even under `-stdlib=libc++`; the top-level CMakeLists handles this by pushing `-stdlib=libc++` into the initial flags before `project()`.

```bash
cmake --preset libcxx
cmake --build --preset libcxx-debug
ctest --preset libcxx-debug
```

## Cross-compiling for Windows

Windows x86_64 binaries can be built from a Linux host with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang + libc++, including the
C++26 standard-library module, so `import std;` works on the target). Keep the release
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

Set `-DCLANG_TIDY_ENABLED=ON` (default) to enable static analysis when `clang-tidy` is found and a `.clang-tidy` file is present at the project root. Set it to `OFF` to skip configuring Clang-Tidy even if available. Clang-Tidy's handling of `import std;` and module units depends on the installed version; if it cannot process a module TU, disable it for that target rather than treating the diagnostic as a source error.
