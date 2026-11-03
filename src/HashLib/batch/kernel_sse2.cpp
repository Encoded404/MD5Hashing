// SSE2 kernel: four independent messages per call. This is the x86-64
// baseline, so it is always available once the dispatcher has run.

module;
#include <xsimd/xsimd.hpp>

module hashlib.batch;

import std;

static_assert(std::endian::native == std::endian::little,
              "the SIMD kernels load MD5's little-endian words directly");

namespace hashlib::detail {

void CompressSse2(const std::byte* const* blocks, std::uint32_t* state) noexcept {
    CompressBlock<xsimd::batch<std::uint32_t, xsimd::sse2>>(blocks, state);
}

}  // namespace hashlib::detail
