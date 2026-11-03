// AVX-512F kernel: sixteen independent messages per call. -mavx2 is enabled as
// well, because the 16x16 transpose is built from two 8x8 AVX2 transposes
// (xsimd 13 has no 32-bit AVX-512 transpose).

module;
#include <xsimd/xsimd.hpp>

module hashlib.batch;

import std;

static_assert(std::endian::native == std::endian::little,
              "the SIMD kernels load MD5's little-endian words directly");

namespace hashlib::detail {
namespace {

// The one ISA-specific piece of the algorithm. Transpose lanes 0..7 and 8..15
// separately with AVX2, then join each pair of 256-bit rows into one zmm.
struct Avx512ScheduleLoader {
    void operator()(const std::byte* const* blocks,
                    xsimd::batch<std::uint32_t, xsimd::avx512f> (&m)[16]) const noexcept {
        using Half = xsimd::batch<std::uint32_t, xsimd::avx2>;
        using Full = xsimd::batch<std::uint32_t, xsimd::avx512f>;

        Half low0[8];
        Half low1[8];
        Half high0[8];
        Half high1[8];
        for (std::size_t lane = 0; lane < 8; ++lane) {
            const auto* low = reinterpret_cast<const std::uint32_t*>(blocks[lane]);
            const auto* high = reinterpret_cast<const std::uint32_t*>(blocks[lane + 8]);
            low0[lane] = Half::load_unaligned(low);
            low1[lane] = Half::load_unaligned(low + 8);
            high0[lane] = Half::load_unaligned(high);
            high1[lane] = Half::load_unaligned(high + 8);
        }

        xsimd::transpose(low0, low0 + 8);
        xsimd::transpose(low1, low1 + 8);
        xsimd::transpose(high0, high0 + 8);
        xsimd::transpose(high1, high1 + 8);

        for (std::size_t j = 0; j < 8; ++j) {
            m[j] = Full(_mm512_inserti64x4(_mm512_castsi256_si512(low0[j].data),
                                           high0[j].data, 1));
            m[j + 8] = Full(_mm512_inserti64x4(_mm512_castsi256_si512(low1[j].data),
                                               high1[j].data, 1));
        }
    }
};

}  // namespace

void CompressAvx512(const std::byte* const* blocks, std::uint32_t* state) noexcept {
    CompressBlock<xsimd::batch<std::uint32_t, xsimd::avx512f>>(
        blocks, state, Avx512ScheduleLoader{});
}

}  // namespace hashlib::detail
