// Runtime kernel selection for md5_batch.
//
// This translation unit is compiled for the baseline ISA: it must run on the
// oldest CPU the binary supports. The per-architecture kernels live in their
// own TUs compiled with -mavx2 / -mavx512f; this file only takes their
// addresses. Selection happens once (the caller caches the result), so the hot
// path never executes CPU detection.

module;

#if defined(HASHLIB_X86) && defined(_MSC_VER) && !defined(__clang__)
#  include <immintrin.h>
#  include <intrin.h>
#endif

module hashlib.batch;

import std;

namespace hashlib::detail {
namespace {

#if defined(HASHLIB_X86)

[[maybe_unused]] bool HaveAvx2() noexcept {
#  if defined(__clang__) || defined(__GNUC__)
    return __builtin_cpu_supports("avx2") != 0;
#  elif defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 7) {
        return false;
    }
    __cpuid(regs, 1);
    if ((regs[2] & (1 << 27)) == 0) {  // OSXSAVE
        return false;
    }
    if ((_xgetbv(0) & 0x6) != 0x6) {  // XMM + YMM state enabled by the OS
        return false;
    }
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;  // AVX2
#  else
    return false;
#  endif
}

[[maybe_unused]] bool HaveAvx512F() noexcept {
#  if defined(__clang__) || defined(__GNUC__)
    // The AVX-512 kernel also uses AVX2 for its 16x16 transpose.
    return __builtin_cpu_supports("avx512f") != 0 &&
           __builtin_cpu_supports("avx2") != 0;
#  elif defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 7) {
        return false;
    }
    __cpuid(regs, 1);
    if ((regs[2] & (1 << 27)) == 0) {  // OSXSAVE
        return false;
    }
    if ((_xgetbv(0) & 0xE6) != 0xE6) {  // XMM/YMM + opmask + ZMM state
        return false;
    }
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 16)) != 0 && (regs[1] & (1 << 5)) != 0;
#  else
    return false;
#  endif
}

#endif  // HASHLIB_X86

struct Entry {
    bool (*supported)() noexcept;
    CompressFn compress;
    std::size_t lanes;
    const char* name;
};

// Best first. Add a new ISA by adding a kernel TU, a declaration in
// batch.cppm, and an entry here.
constexpr Entry kEntries[] = {
#if defined(HASHLIB_WITH_AVX512)
    {&HaveAvx512F, &CompressAvx512, 16, "avx512f"},
#endif
#if defined(HASHLIB_WITH_AVX2)
    {&HaveAvx2, &CompressAvx2, 8, "avx2"},
#endif
#if defined(HASHLIB_X86)
    {[]() noexcept { return true; }, &CompressSse2, 4, "sse2"},
#endif
    {nullptr, nullptr, 1, "scalar"},
};

}  // namespace

BatchKernel SelectBatchKernel() noexcept {
#if defined(HASHLIB_X86) && (defined(__clang__) || defined(__GNUC__))
    // Safe even if a caller reaches the library during static initialisation,
    // before the runtime's own CPU-feature constructor has run.
    __builtin_cpu_init();
#endif

    // Test hook: force one kernel so every path can be exercised on one
    // machine. Unknown names (or an unavailable ISA) fall through to scalar.
    const char* forced = std::getenv("HASHLIB_FORCE_ISA");
    const std::string_view forcedName =
        forced != nullptr ? std::string_view{forced} : std::string_view{};

    for (const Entry& entry : kEntries) {
        if (entry.compress == nullptr) {
            break;
        }
        if (forcedName.empty() || forcedName == entry.name) {
            if (entry.supported()) {
                return BatchKernel{entry.compress, entry.lanes, entry.name};
            }
        }
    }

    return BatchKernel{};
}

}  // namespace hashlib::detail
