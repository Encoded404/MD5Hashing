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

/// The kernels of the widest ISA the CPU supports, narrowest batch first. The
/// scalar fallback is always present, so `count` is never zero.
struct KernelSet {
    std::array<BatchKernel, 3> kernels{};
    std::size_t count = 0;
};

/// Selects the kernels supported by the CPU executing this process.
/// Compiled for the baseline ISA: it must run before any AVX code can.
[[nodiscard]] KernelSet SelectBatchKernels() noexcept;

/// Compresses `blockCount` whole blocks that already carry the MD5 padding, chaining them into `state`, one block at a time and without any SIMD.
/// The scalar fallback of `md5_batch` replays the message through `md5_fast`, which pads; a padded block carries no message length to replay, so this is what `md5_batch_padded` uses when the runtime selected no batch kernel.
void CompressPaddedScalar(const std::byte* blocks, std::size_t blockCount, std::uint32_t* state) noexcept;

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

// The step body below visits the four state words in the order a, d, c, b, and
// feeds the rounds the message word the RFC names: j in round 1, then
// (5j + 1) % 16, (3j + 5) % 16 and (7j) % 16. Both are functions of the step
// index, so the body stays free of run-time indices while being written once
// for any number of interleaved streams.
constexpr std::size_t RoleOf(std::size_t step) noexcept { return (4 - step % 4) % 4; }

constexpr std::size_t WordOf(std::size_t step) noexcept {
    const std::size_t j = step % 16;
    if (step < 16) {
        return j;
    }
    if (step < 32) {
        return (5 * j + 1) % 16;
    }
    if (step < 48) {
        return (3 * j + 5) % 16;
    }
    return (7 * j) % 16;
}

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

// The 64-step fold has to stay inside the kernel. Out of line it becomes a
// function that receives the state and the schedules by reference, so every
// value goes through memory and the interleaving gain disappears. `inline`
// alone is not enough: Clang outlines the four-stream body because of its size.
#if defined(_MSC_VER) && !defined(__clang__)
#  define HASHLIB_ALWAYS_INLINE __forceinline
#else
#  define HASHLIB_ALWAYS_INLINE inline __attribute__((always_inline))
#endif

// One step of the batch body, applied to every stream. The streams are
// independent, so their steps can be issued back to back and the processor
// overlaps the dependency chains instead of waiting on one at a time.
template <std::size_t I, class Batch, std::size_t Streams>
HASHLIB_ALWAYS_INLINE void StepAll(Batch (&state)[Streams][4],
                                   const Batch (&message)[Streams][16]) noexcept {
    constexpr std::size_t role = RoleOf(I);
    constexpr std::size_t word = WordOf(I);
    for (std::size_t stream = 0; stream < Streams; ++stream) {
        Batch* words = state[stream];
        words[role] = Step<I>(words[role], words[(role + 1) % 4], words[(role + 2) % 4],
                              words[(role + 3) % 4], message[stream][word]);
    }
}

template <class Batch, std::size_t Streams, std::size_t... I>
HASHLIB_ALWAYS_INLINE void Steps(Batch (&state)[Streams][4],
                                 const Batch (&message)[Streams][16],
                                 std::index_sequence<I...>) noexcept {
    (StepAll<I>(state, message), ...);
}

#undef HASHLIB_ALWAYS_INLINE

// Compress one 64-byte block from every lane into the interleaved state:
// a[lanes], b[lanes], c[lanes], d[lanes], where lanes = Streams * Batch::size.
//
// The call carries Streams independent groups: group s owns lanes
// [s*N, s*N + N) and the blocks[s*N .. s*N + N). The groups never interact, so
// the digests are identical to hashing each group in its own call. What changes
// is the schedule: one group's dependency chain leaves the vector ALUs idle,
// and the other groups fill that time. A kernel whose best transpose is
// ISA-specific passes its own loader as the third template argument; the
// 64-step body stays shared.
template <class Batch, std::size_t Streams = 1, class Loader = TransposeLoader<Batch>>
inline void CompressBlock(const std::byte* const* blocks, std::uint32_t* state,
                          Loader load = {}) noexcept {
    constexpr std::size_t N = Batch::size;
    constexpr std::size_t L = Streams * N;
    static_assert(Streams > 0, "a batch needs at least one stream");

    Batch w[Streams][4];
    Batch m[Streams][16];

    for (std::size_t stream = 0; stream < Streams; ++stream) {
        for (std::size_t role = 0; role < 4; ++role) {
            w[stream][role] = Batch::load_unaligned(state + role * L + stream * N);
        }
        load(blocks + stream * N, m[stream]);
    }

    Batch initial[Streams][4];
    for (std::size_t stream = 0; stream < Streams; ++stream) {
        for (std::size_t role = 0; role < 4; ++role) {
            initial[stream][role] = w[stream][role];
        }
    }

    Steps(w, m, std::make_index_sequence<64>{});

    for (std::size_t stream = 0; stream < Streams; ++stream) {
        for (std::size_t role = 0; role < 4; ++role) {
            w[stream][role] += initial[stream][role];
            w[stream][role].store_unaligned(state + role * L + stream * N);
        }
    }
}

// Kernel entry points. Definitions live in the per-ISA implementation units;
// the dispatcher takes their addresses. Declaring all of them unconditionally
// keeps this interface free of build-configuration #if blocks.
//
// The suffix is the stream count: `x2` interleaves two groups of batches, so it
// hashes twice as many messages per call, `x4` four. The number of lanes is the
// ISA width times that count.
void CompressSse2(const std::byte* const* blocks, std::uint32_t* state) noexcept;
void CompressAvx2(const std::byte* const* blocks, std::uint32_t* state) noexcept;
void CompressAvx2x2(const std::byte* const* blocks, std::uint32_t* state) noexcept;
void CompressAvx512(const std::byte* const* blocks, std::uint32_t* state) noexcept;
void CompressAvx512x2(const std::byte* const* blocks, std::uint32_t* state) noexcept;
void CompressAvx512x4(const std::byte* const* blocks, std::uint32_t* state) noexcept;

}  // namespace hashlib::detail
