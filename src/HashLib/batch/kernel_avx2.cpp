// AVX2 kernels: one or two independent groups of eight messages per call.
// Two groups reach the throughput of one sixteen-lane AVX-512 group, so a
// machine without AVX-512 still gets a second dependency chain to overlap.

module;
#include <xsimd/xsimd.hpp>

module hashlib.batch;

import std;

static_assert(std::endian::native == std::endian::little,
              "the SIMD kernels load MD5's little-endian words directly");

namespace hashlib::detail {
namespace {

using Batch256 = xsimd::batch<std::uint32_t, xsimd::avx2>;

}  // namespace

void CompressAvx2(const std::byte* const* blocks, std::uint32_t* state) noexcept {
    CompressBlock<Batch256>(blocks, state);
}

void CompressAvx2x2(const std::byte* const* blocks, std::uint32_t* state) noexcept {
    CompressBlock<Batch256, 2>(blocks, state);
}

}  // namespace hashlib::detail
