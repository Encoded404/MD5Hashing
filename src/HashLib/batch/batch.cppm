export module hashlib.batch;

// Internal module: MD5 compression for many independent messages in SIMD lanes.
//
// The interface is compiled for the baseline ISA. Each kernel is a separate
// module implementation unit compiled with the flags of the ISA it targets, so
// one binary can choose a kernel on the CPU that runs it. Implementation units
// emit no BMI, which is what lets this baseline interface be imported by the
// dispatcher alongside AVX2/AVX-512 code (Clang rejects importing a BMI that
// was built with newer target features).
//
// The 64-step body deliberately does not mention xsimd. It only uses
// operations found by ADL on the batch type that a kernel instantiates it
// with: load_unaligned, rotl, transpose, and the vector operators.

import std;

export namespace hashlib::detail {

/// Compresses one 64-byte block per lane into an interleaved state.
using CompressFn = void (*)(const std::byte* const* blocks,
                            std::uint32_t* state) noexcept;

struct BatchKernel {
    CompressFn compress = nullptr;  // nullptr selects the scalar path
    std::size_t lanes = 1;
    const char* name = "scalar";
};

/// Selects the best kernel supported by the CPU executing this process.
/// Compiled for the baseline ISA: it must run before any AVX code can.
[[nodiscard]] BatchKernel SelectBatchKernel() noexcept;

}  // namespace hashlib::detail

namespace hashlib::detail {

// RFC 1321: K[i] = floor(2^32 * |sin(i + 1)|).
inline constexpr std::uint32_t kRoundConstants[64] = {
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU,
    0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
    0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U,
    0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
    0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U,
    0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
    0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
    0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
    0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U,
    0xffeff47dU, 0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
    0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U
};

// Left-rotate amount per step: four steps per round, repeated over 16 steps.
inline constexpr std::uint32_t kRotations[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

// One MD5 step:
//
//   target = rotl(target + K[I] + message + F(x, y, z), s[I]) + x
//
// The caller passes the four state words in their rotated roles. That keeps
// this function free of the role-selection logic (and its ternaries) that the
// scalar single-message code needs, and makes each round read like RFC 1321.
template <std::size_t I, class Batch>
[[nodiscard]] inline Batch Step(Batch target, const Batch& x, const Batch& y,
                                const Batch& z, const Batch& message) noexcept {
    constexpr std::size_t round = I / 16;
    constexpr std::uint32_t k = kRoundConstants[I];
    constexpr int shift = static_cast<int>(kRotations[I]);

    Batch f;
    if constexpr (round == 0) {
        f = z ^ (x & (y ^ z));
    } else if constexpr (round == 1) {
        f = y ^ (z & (x ^ y));
    } else if constexpr (round == 2) {
        f = x ^ y ^ z;
    } else {
        f = y ^ (x | ~z);
    }

    // K + message is off the rotate's dependency chain.
    const Batch messagePlusConstant = Batch(k) + message;
    return rotl((target + messagePlusConstant) + f, shift) + x;
}

// Transpose N lane blocks into 16 message-word batches. xsimd supplies an
// efficient transpose for SSE2 (4x4) and AVX2 (8x8). AVX-512 has no 32-bit
// transpose in xsimd 13, so that kernel supplies its own loader below.
template <class Batch>
inline void LoadSchedule(const std::byte* const* blocks, Batch (&m)[16]) noexcept {
    constexpr std::size_t N = Batch::size;
    static_assert(N == 4 || N == 8 || N == 16, "unsupported batch width");
    constexpr std::size_t groups = 16 / N;

    for (std::size_t group = 0; group < groups; ++group) {
        Batch rows[N];
        for (std::size_t lane = 0; lane < N; ++lane) {
            const auto* words = reinterpret_cast<const std::uint32_t*>(blocks[lane]);
            rows[lane] = Batch::load_unaligned(words + group * N);
        }
        transpose(rows, rows + N);
        for (std::size_t j = 0; j < N; ++j) {
            m[group * N + j] = rows[j];
        }
    }
}

template <class Batch>
struct TransposeLoader {
    void operator()(const std::byte* const* blocks, Batch (&m)[16]) const noexcept {
        LoadSchedule(blocks, m);
    }
};

// Compress one 64-byte block from every lane into the interleaved state:
// a[lanes], b[lanes], c[lanes], d[lanes]. A kernel whose best transpose is
// ISA-specific passes its own loader as the second template argument; the
// 64-step body stays shared.
template <class Batch, class Loader = TransposeLoader<Batch>>
inline void CompressBlock(const std::byte* const* blocks, std::uint32_t* state,
                          Loader load = {}) noexcept {
    constexpr std::size_t N = Batch::size;

    Batch a = Batch::load_unaligned(state + 0 * N);
    Batch b = Batch::load_unaligned(state + 1 * N);
    Batch c = Batch::load_unaligned(state + 2 * N);
    Batch d = Batch::load_unaligned(state + 3 * N);

    Batch m[16];
    load(blocks, m);

    const Batch a0 = a;
    const Batch b0 = b;
    const Batch c0 = c;
    const Batch d0 = d;

    // Round 1: F(x, y, z) = z ^ (x & (y ^ z)); words 0..15 in order.
    a = Step<0>(a, b, c, d, m[0]);
    d = Step<1>(d, a, b, c, m[1]);
    c = Step<2>(c, d, a, b, m[2]);
    b = Step<3>(b, c, d, a, m[3]);
    a = Step<4>(a, b, c, d, m[4]);
    d = Step<5>(d, a, b, c, m[5]);
    c = Step<6>(c, d, a, b, m[6]);
    b = Step<7>(b, c, d, a, m[7]);
    a = Step<8>(a, b, c, d, m[8]);
    d = Step<9>(d, a, b, c, m[9]);
    c = Step<10>(c, d, a, b, m[10]);
    b = Step<11>(b, c, d, a, m[11]);
    a = Step<12>(a, b, c, d, m[12]);
    d = Step<13>(d, a, b, c, m[13]);
    c = Step<14>(c, d, a, b, m[14]);
    b = Step<15>(b, c, d, a, m[15]);

    // Round 2: G(x, y, z) = y ^ (z & (x ^ y)); word (5 * step + 1) % 16.
    a = Step<16>(a, b, c, d, m[1]);
    d = Step<17>(d, a, b, c, m[6]);
    c = Step<18>(c, d, a, b, m[11]);
    b = Step<19>(b, c, d, a, m[0]);
    a = Step<20>(a, b, c, d, m[5]);
    d = Step<21>(d, a, b, c, m[10]);
    c = Step<22>(c, d, a, b, m[15]);
    b = Step<23>(b, c, d, a, m[4]);
    a = Step<24>(a, b, c, d, m[9]);
    d = Step<25>(d, a, b, c, m[14]);
    c = Step<26>(c, d, a, b, m[3]);
    b = Step<27>(b, c, d, a, m[8]);
    a = Step<28>(a, b, c, d, m[13]);
    d = Step<29>(d, a, b, c, m[2]);
    c = Step<30>(c, d, a, b, m[7]);
    b = Step<31>(b, c, d, a, m[12]);

    // Round 3: H(x, y, z) = x ^ y ^ z; word (3 * step + 5) % 16.
    a = Step<32>(a, b, c, d, m[5]);
    d = Step<33>(d, a, b, c, m[8]);
    c = Step<34>(c, d, a, b, m[11]);
    b = Step<35>(b, c, d, a, m[14]);
    a = Step<36>(a, b, c, d, m[1]);
    d = Step<37>(d, a, b, c, m[4]);
    c = Step<38>(c, d, a, b, m[7]);
    b = Step<39>(b, c, d, a, m[10]);
    a = Step<40>(a, b, c, d, m[13]);
    d = Step<41>(d, a, b, c, m[0]);
    c = Step<42>(c, d, a, b, m[3]);
    b = Step<43>(b, c, d, a, m[6]);
    a = Step<44>(a, b, c, d, m[9]);
    d = Step<45>(d, a, b, c, m[12]);
    c = Step<46>(c, d, a, b, m[15]);
    b = Step<47>(b, c, d, a, m[2]);

    // Round 4: I(x, y, z) = y ^ (x | ~z); word (7 * step) % 16.
    a = Step<48>(a, b, c, d, m[0]);
    d = Step<49>(d, a, b, c, m[7]);
    c = Step<50>(c, d, a, b, m[14]);
    b = Step<51>(b, c, d, a, m[5]);
    a = Step<52>(a, b, c, d, m[12]);
    d = Step<53>(d, a, b, c, m[3]);
    c = Step<54>(c, d, a, b, m[10]);
    b = Step<55>(b, c, d, a, m[1]);
    a = Step<56>(a, b, c, d, m[8]);
    d = Step<57>(d, a, b, c, m[15]);
    c = Step<58>(c, d, a, b, m[6]);
    b = Step<59>(b, c, d, a, m[13]);
    a = Step<60>(a, b, c, d, m[4]);
    d = Step<61>(d, a, b, c, m[11]);
    c = Step<62>(c, d, a, b, m[2]);
    b = Step<63>(b, c, d, a, m[9]);

    a += a0;
    b += b0;
    c += c0;
    d += d0;

    a.store_unaligned(state + 0 * N);
    b.store_unaligned(state + 1 * N);
    c.store_unaligned(state + 2 * N);
    d.store_unaligned(state + 3 * N);
}

// Kernel entry points. Definitions live in the per-ISA implementation units;
// the dispatcher takes their addresses. Declaring all three unconditionally
// keeps this interface free of build-configuration #if blocks.
void CompressSse2(const std::byte* const* blocks, std::uint32_t* state) noexcept;
void CompressAvx2(const std::byte* const* blocks, std::uint32_t* state) noexcept;
void CompressAvx512(const std::byte* const* blocks, std::uint32_t* state) noexcept;

}  // namespace hashlib::detail
