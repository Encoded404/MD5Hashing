// AVX2 kernel: eight independent messages per call.

module;
#include <xsimd/xsimd.hpp>

module hashlib.batch;

import std;

static_assert(std::endian::native == std::endian::little,
              "the SIMD kernels load MD5's little-endian words directly");

namespace hashlib::detail {

void CompressAvx2(const std::byte* const* blocks, std::uint32_t* state) noexcept {
    CompressBlock<xsimd::batch<std::uint32_t, xsimd::avx2>>(blocks, state);
}

}  // namespace hashlib::detail
