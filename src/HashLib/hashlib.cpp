module;

#include <logging/logging_macros.hpp>

module hashlib;

import std;
import logiface;
import hashlib.utilities;

namespace hashlib {

Digest naive_md5(std::span<const std::byte> input) {
    LOGIFACE_LOG(trace, "hashlib::naive_md5 called with " + std::to_string(input.size()) + " byte(s)");

    std::uint64_t lengthInBits = (input.size() * 8UL); //  get the length in bits

    std::uint16_t bitsToAppend = (447 - lengthInBits) % 512; // pads to 512 - 64 = 448
    std::vector<std::byte> buffer(input.size() + (8 + bitsToAppend) / 8 + 8); // 8 bits for the 10000000. bitsToAppend for the padding. and the final 8 bits for the length in uint64_t

    LOGIFACE_LOG(trace, "input was " + std::to_string(input.size()) + " byte(s) (" + std::to_string(lengthInBits) + " bits), and had " + std::to_string((bitsToAppend + 8) / 8) + " byte(s) added (" + std::to_string((bitsToAppend + 8)) + " bits)");

    buffer.assign_range(input);
    buffer.push_back(static_cast<std::byte>(0x80)); // append 10000000 to the end.
    for (std::uint16_t i = 0; i < bitsToAppend / 8; i++) {
        buffer.push_back(static_cast<std::byte>(0x00)); // append 00000000 to the end.
    }

    // append the original length to the end
    buffer.append_range(std::bit_cast<std::array<std::byte, 8>>(lengthInBits));

    std::uint32_t A = 0x67452301;
    std::uint32_t B = 0xefcdab89;
    std::uint32_t C = 0x98badcfe;
    std::uint32_t D = 0x10325476;

    std::size_t blocks = buffer.size() / (512>>3); // get how many 512 bit blocks there are.
    for (std::size_t chunk = 0; chunk < blocks; chunk++) {

        std::array<std::byte, 64> block{};
        std::memcpy(block.data(), buffer.data() + chunk * 64, 64);

        auto chunks = std::bit_cast<std::array<std::uint32_t, 16>>(block);

        std::uint32_t A_inner = A;
        std::uint32_t B_inner = B;
        std::uint32_t C_inner = C;
        std::uint32_t D_inner = D;

        for (std::uint8_t i = 0; i < 64; i++) {
            constexpr std::array<std::uint32_t, 64> K{
                0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
                0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
                0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
                0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
                0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
                0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
                0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
                0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
                0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
                0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
                0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
                0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
                0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
                0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
                0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
                0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
            };

            std::uint32_t F = 0;
            std::uint32_t g = 0;

            if (i <= 15) {
                F = (B_inner & C_inner) | ((~B_inner) & D_inner);
                g = i;
            }
            else if (i <= 31) {
                F = (D_inner & B_inner) | ((~D_inner) & C_inner);
                g = (5*i + 1) % 16;
            }
            else if (i <= 47) {
                F = B_inner ^ C_inner ^ D_inner;
                g = (3 * i + 5) % 16;
            }
            else if (i <= 63) {
                F = C_inner ^ (B_inner | (~D_inner));
                g = (7 * i) % 16;
            }

            F = F + A_inner + K[i] + chunks[g];
            A_inner = D_inner;
            D_inner = C_inner;
            C_inner = B_inner;

            constexpr std::array<std::uint32_t, 64> s{
                7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
                5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
                4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
                6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
            };

            B_inner = B_inner + std::rotl(F, s[i]);
        }

        A += A_inner;
        B += B_inner;
        C += C_inner;
        D += D_inner;
    }

    const Digest result = utilities::PackValues(A, B, C, D);
    return result;
}

// md5_fast: the optimised single-message implementation.
//
// Same digest as naive_md5. Deliberately no logging and no allocation:
// the point of this variant is that nothing here can allocate, throw or touch a lock, so a level check or a to_string() would defeat it.
// The measurements behind each choice (state by value, always_inline fold, std::rotl, ternary F, no prefetch) are in docs/md5-optimization.md.

#if defined(_MSC_VER)
#define HASHLIB_ALWAYS_INLINE __forceinline
#define HASHLIB_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define HASHLIB_ALWAYS_INLINE inline __attribute__((always_inline))
#define HASHLIB_NOINLINE __attribute__((noinline))
#else
#define HASHLIB_ALWAYS_INLINE inline
#define HASHLIB_NOINLINE inline
#endif

namespace {

    constexpr std::size_t kBlockBytes = 64;

    struct State {
        std::uint32_t a = 0x67452301U;
        std::uint32_t b = 0xefcdab89U;
        std::uint32_t c = 0x98badcfeU;
        std::uint32_t d = 0x10325476U;
    };

    // RFC 1321: K[i] = floor(2^32 * |sin(i + 1)|). Spelled out rather than computed
    // so the table is exactly the published constants.
    constexpr std::array<std::uint32_t, 64> kRoundConstants {
        0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U,
        0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U,
        0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU,
        0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
        0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U,
        0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
        0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U,
        0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
        0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU,
        0xeb86d391U
    };

    // Left-rotate amount per step: four steps per round, repeated over 16 steps.
    constexpr std::array<std::uint32_t, 64> kRotations {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
        14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };

    // Message-word index per step, derived rather than typed out.
    constexpr std::array<std::uint32_t, 64> kMessageIndex = [] {
        std::array<std::uint32_t, 64> index{};
        for (std::size_t i = 0; i < 64; ++i) {
            const std::size_t round = i / 16;
            const std::size_t step = i % 16;
            if (round == 0) {
                index[i] = static_cast<std::uint32_t>(step);
            } else if (round == 1) {
                index[i] = static_cast<std::uint32_t>((5 * step + 1) % 16);
            } else if (round == 2) {
                index[i] = static_cast<std::uint32_t>((3 * step + 5) % 16);
            } else {
                index[i] = static_cast<std::uint32_t>((7 * step) % 16);
            }
        }
        return index;
    }();

    // MD5 is defined little-endian; the memcpy folds into a single unaligned load.
    [[nodiscard]] std::uint32_t LoadLe32(const std::byte* bytes) noexcept {
        std::uint32_t value;
        std::memcpy(&value, bytes, sizeof(value));
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        return value;
    }

    void StoreLe32(std::byte* bytes, std::uint32_t value) noexcept {
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        std::memcpy(bytes, &value, sizeof(value));
    }

    void StoreLe64(std::byte* bytes, std::uint64_t value) noexcept {
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        std::memcpy(bytes, &value, sizeof(value));
    }

    // Identity, plus an optimisation barrier: the value is forced into a register
    // instead of being folded into the expression that produced it.
    //
    // MD5's cost is its round-to-round latency, and the optimiser is free to reassociate `target + (k + X[g]) + f`.
    // LLVM does exactly that: it moves the constant behind `f`,
    // where it becomes one more dependent operation on the chain that ends in the rotate.
    // Pinning the sum first keeps the constant and the message word off that chain.
    // The barrier emits no instructions; it only constrains the optimiser.
    // 5.98 -> 4.98 cycles per byte on the shipped Clang build (0.92 -> 1.11 GB/s
    // for a 1 MiB message, at 5.5 GHz). Every -march=native configuration lands
    // on the same 4.75 cycles per step afterwards, whichever compiler and
    // -mtune it is built with.
    // See docs/md5-optimization.md.
    //
    // Compilers with no inline-asm barrier get the value unchanged: correct, just
    // without the scheduling hint.
    //
    // Because the barrier is volatile, this function is no longer a pure
    // expression to the optimiser. That is right for real inputs, but it also
    // means a call in a loop that hashes one invariant buffer is no longer
    // hoisted out, while the older non-volatile version was. A benchmark that
    // compares the two, or that just wants to measure the hash, therefore needs
    // an opaque function pointer or input that varies per iteration.
    [[nodiscard]] HASHLIB_ALWAYS_INLINE std::uint32_t Pin(std::uint32_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        asm volatile("" : "+r"(value));
#endif
        return value;
    }

    // One MD5 step. The four state words rotate through the (target, x, y, z) roles with I % 4, which is what makes the 64 steps a straight-line sequence.
    // step I writes the slot that step I - 1 read as x, so no register shuffling is needed.
    template <std::size_t I>
    inline void Step(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d, const std::byte* block) noexcept {
        constexpr std::size_t round = I / 16;
        constexpr std::uint32_t k = kRoundConstants[I];
        constexpr std::uint32_t g = kMessageIndex[I];
        constexpr int shift = static_cast<int>(kRotations[I]);

        std::uint32_t& target = (I % 4 == 0) ? a :
                                (I % 4 == 1) ? d :
                                (I % 4 == 2) ? c :
                                b;
        std::uint32_t& x =  (I % 4 == 0) ? b :
                            (I % 4 == 1) ? a :
                            (I % 4 == 2) ? d :
                            c;
        std::uint32_t& y =  (I % 4 == 0) ? c :
                            (I % 4 == 1) ? b :
                            (I % 4 == 2) ? a :
                            d;
        std::uint32_t& z =  (I % 4 == 0) ? d :
                            (I % 4 == 1) ? c :
                            (I % 4 == 2) ? b :
                            a;

        // The ternary forms use the same number of operations as (x & y) | (~x & z) but keep the complement off the critical path.
        std::uint32_t f = 0;
        if constexpr (round == 0) {
            f = z ^ (x & (y ^ z));
        } else if constexpr (round == 1) {
            f = y ^ (z & (x ^ y));
        } else if constexpr (round == 2) {
            f = x ^ y ^ z;
        } else {
            f = y ^ (x | ~z);
        }

        // k + X[g] is combined first and pinned, so the optimiser cannot move the
        // constant behind f and add a dependent op to the rotate's chain.
        // this is what it does in normal code:
        //        target += f + k + LoadLe32(block + 4 * g);
        //        target = std::rotl(target, shift);
        //        target += x;

        const std::uint32_t message = Pin(k + LoadLe32(block + 4 * g));
        target = std::rotl((target + message) + f, shift) + x;
    }

    // A function template rather than a generic lambda: GCC declines to inline a 600-instruction closure,
    // and the by-reference captures then force the state through memory, which measures 25 % slower.
    // See docs/md5-optimization.md.
    template <std::size_t... I>
    HASHLIB_ALWAYS_INLINE void RunSteps(std::index_sequence<I...>, std::uint32_t& a,
                                        std::uint32_t& b, std::uint32_t& c, std::uint32_t& d,
                                        const std::byte* block) noexcept {
        (Step<I>(a, b, c, d, block), ...);
    }

    // Out of line on purpose.
    // one copy of the 64-step body instead of one per call site, and taking the state by value keeps a, b, c, d in registers across the whole message instead of round-tripping them through memory once per block.
    HASHLIB_NOINLINE State Compress(State state, const std::byte* blocks, std::size_t blockCount) noexcept {
        std::uint32_t a = state.a;
        std::uint32_t b = state.b;
        std::uint32_t c = state.c;
        std::uint32_t d = state.d;

        for (std::size_t block = 0; block < blockCount; ++block, blocks += kBlockBytes) {
            const std::uint32_t aBeforeBlock = a;
            const std::uint32_t bBeforeBlock = b;
            const std::uint32_t cBeforeBlock = c;
            const std::uint32_t dBeforeBlock = d;

            RunSteps(std::make_index_sequence<64>{}, a, b, c, d, blocks);

            a += aBeforeBlock;
            b += bBeforeBlock;
            c += cBeforeBlock;
            d += dBeforeBlock;
        }

        return State{a, b, c, d};
    }

}  // anonymous namespace

Digest md5_fast(std::span<const std::byte> input) noexcept {
    const std::size_t blockCount = input.size() / kBlockBytes;
    const std::size_t remainder = input.size() - blockCount * kBlockBytes;

    // Full blocks are hashed straight out of the caller's buffer: no copy.
    // Compress is out of line, so skip the call entirely for sub-block messages.
    State state;
    if (blockCount != 0) {
        state = Compress(State{}, input.data(), blockCount);
    }

    // Only the trailing one or two blocks go through a zeroed stack buffer.
    std::byte tail[2 * kBlockBytes] = {};
    if (remainder != 0) {
        std::memcpy(tail, input.data() + blockCount * kBlockBytes, remainder);
    }
    tail[remainder] = std::byte{0x80};
    const std::size_t tailBlocks = (remainder + 9 <= kBlockBytes) ? 1 : 2;  // 0x80 + 8-byte length
    StoreLe64(tail + tailBlocks * kBlockBytes - 8, static_cast<std::uint64_t>(input.size()) * 8);

    state = Compress(state, tail, tailBlocks);

    Digest digest{};
    StoreLe32(digest.data() + 0, state.a);
    StoreLe32(digest.data() + 4, state.b);
    StoreLe32(digest.data() + 8, state.c);
    StoreLe32(digest.data() + 12, state.d);
    return digest;
}

#undef HASHLIB_ALWAYS_INLINE
#undef HASHLIB_NOINLINE

}  // namespace hashlib
