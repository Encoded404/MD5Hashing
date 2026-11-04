// AVX-512F kernels: one, two or four independent groups of sixteen messages per
// call. The kernel needs only AVX-512F, but it is built with AVX2 enabled, and
// the dispatcher checks for AVX2 too, so that auto-vectorisation can never emit
// an instruction the dispatcher did not account for.

module;
#include <xsimd/xsimd.hpp>

module hashlib.batch;

import std;

static_assert(std::endian::native == std::endian::little,
              "the SIMD kernels load MD5's little-endian words directly");

namespace hashlib::detail {
namespace {

using Batch512 = xsimd::batch<std::uint32_t, xsimd::avx512f>;

constexpr std::size_t kLanes = Batch512::size;

// One index pattern per stage and per value of the bit that stage swaps.
// Each pattern gives, for every output word, the source word: 0..15 selects from the first register of the pair and 16..31 from the second.
// A stage swaps bit `stage` of the lane number with bit `stage` of the word number, so after the last stage word j of every lane has moved into lane j's position.
constexpr std::array<std::array<int, kLanes>, 8> MakeTransposeIndex() {
    std::array<std::array<int, kLanes>, 8> table{};
    for (std::size_t stage = 0; stage < 4; ++stage) {
        const int bit = 1 << stage;
        for (int high = 0; high < 2; ++high) {
            for (int word = 0; word < static_cast<int>(kLanes); ++word) {
                const bool own = ((word & bit) != 0) == (high != 0);
                table[2 * stage + high][word] = own ? word : static_cast<int>(kLanes) + (word ^ bit);
            }
        }
    }
    return table;
}

// The patterns are 64 bytes each, so an aligned load fetches one whole vector, and the whole table costs eight loads per transpose.
alignas(64) constexpr std::array<std::array<int, kLanes>, 8> kTransposeIndex = MakeTransposeIndex();

// One stage of the transpose: swap bit kStage of the lane number with bit kStage of the word number.
// Both the stage and the lane are template arguments, so the index choice is a constant and the whole transpose is straight-line code; a rolled lane loop at -O2 kept the array in memory and reloaded every register around each permute.
template <std::size_t kStage, std::size_t... kLaneIndex>
inline void TransposeStage(__m512i (&source)[kLanes], __m512i (&target)[kLanes], std::index_sequence<kLaneIndex...>) noexcept {
    constexpr std::size_t kBit = 1 << kStage;
    const __m512i ownIndex = _mm512_load_si512(kTransposeIndex[2 * kStage + 0].data());
    const __m512i otherIndex = _mm512_load_si512(kTransposeIndex[2 * kStage + 1].data());
    ((target[kLaneIndex] = _mm512_permutex2var_epi32(source[kLaneIndex], (kLaneIndex & kBit) == 0 ? ownIndex : otherIndex, source[kLaneIndex ^ kBit])), ...);
}

template <std::size_t kStage>
inline void TransposeStage(__m512i (&source)[kLanes], __m512i (&target)[kLanes]) noexcept {
    TransposeStage<kStage>(source, target, std::make_index_sequence<kLanes>{});
}

// Transposes sixteen 32-bit words across sixteen lanes: element j of register r becomes element r of register j.
// Four stages of one two-source permute per register beats the two 8x8 AVX2 transposes and the join that this replaces, and it produces each word vector directly in the register the kernel wants.
// The buffers alternate, so the last stage lands back in the caller's array.
inline void TransposeWords(__m512i (&v)[kLanes]) noexcept {
    static_assert(kLanes == 16, "the transpose is written for sixteen lanes");
    __m512i scratch[kLanes];
    TransposeStage<0>(v, scratch);
    TransposeStage<1>(scratch, v);
    TransposeStage<2>(v, scratch);
    TransposeStage<3>(scratch, v);
}

// The one ISA-specific piece of the algorithm: load one whole 64-byte block per lane, then transpose it into one vector of message words per position.
// The loader sees one group of sixteen lanes; CompressBlock calls it once per stream, with the block pointers advanced by sixteen each time.
struct Avx512ScheduleLoader {
    void operator()(const std::byte* const* blocks, Batch512 (&m)[16]) const noexcept {
        __m512i words[kLanes];
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            words[lane] = _mm512_loadu_si512(blocks[lane]);
        }
        TransposeWords(words);
        for (std::size_t word = 0; word < kLanes; ++word) {
            m[word] = Batch512(words[word]);
        }
    }
};

}  // namespace

void CompressAvx512(const std::byte* const* blocks, std::uint32_t* state) noexcept {
    CompressBlock<Batch512>(blocks, state, Avx512ScheduleLoader{});
}

void CompressAvx512x2(const std::byte* const* blocks, std::uint32_t* state) noexcept {
    CompressBlock<Batch512, 2>(blocks, state, Avx512ScheduleLoader{});
}

void CompressAvx512x4(const std::byte* const* blocks, std::uint32_t* state) noexcept {
    CompressBlock<Batch512, 4>(blocks, state, Avx512ScheduleLoader{});
}

}  // namespace hashlib::detail
