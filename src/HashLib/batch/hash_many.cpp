// The many-message entry point. This is an implementation unit of `hashlib`: md5_fast and the public types come from the module interface, while the compression kernels come from the internal hashlib.batch module.

module;

#include <cassert>

module hashlib;

import std;
import hashlib.utilities;
import hashlib.batch;

namespace hashlib {
namespace {

constexpr std::size_t kBlockBytes = 64;
// The widest kernel the dispatcher can return compresses 64 messages per call.
constexpr std::size_t kMaxLanes = 64;

// MD5 initial state in the order the kernels read it: a, b, c, d.
constexpr std::array<std::uint32_t, 4> kInitialState{0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U};

std::uint32_t LoadLe32(const std::byte* bytes) noexcept {
    std::uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    return value;
}

void StoreLe64(std::byte* bytes, std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    std::memcpy(bytes, &value, sizeof(value));
}

// Copies `n` bytes, 1 <= n <= 63, with moves of a fixed size.
// The second move of each case overlaps the first, so together they cover [0, n).
// The destination must have a whole 64-byte block available for that overlap, and the source is never read past `src + n`.
// A run-time sized `memcpy` would go through the C library, whose size dispatch costs more than the copy itself at these lengths, and it would also make the caller spill live registers across the call.
inline void CopyPrefix(std::byte* dst, const std::byte* src, std::size_t n) noexcept {
    if (n >= 32) {
        std::memcpy(dst, src, 32);
        std::memcpy(dst + n - 32, src + n - 32, 32);
    } else if (n >= 16) {
        std::memcpy(dst, src, 16);
        std::memcpy(dst + n - 16, src + n - 16, 16);
    } else if (n >= 8) {
        std::memcpy(dst, src, 8);
        std::memcpy(dst + n - 8, src + n - 8, 8);
    } else if (n >= 4) {
        std::memcpy(dst, src, 4);
        std::memcpy(dst + n - 4, src + n - 4, 4);
    } else if (n >= 2) {
        std::memcpy(dst, src, 2);
        std::memcpy(dst + n - 2, src + n - 2, 2);
    } else {
        *dst = *src;
    }
}

// The batch helpers are inlined unconditionally.
// At -O2 Clang otherwise outlined the uniform path and kept its lane array in memory, which cost the instrumented build a fifth of the hash phase.
#if defined(_MSC_VER) && !defined(__clang__)
#  define HASHLIB_BATCH_INLINE __forceinline
#else
#  define HASHLIB_BATCH_INLINE inline __attribute__((always_inline))
#endif

// How one message is padded: `full` blocks are hashed straight from the caller's buffer, then one or two tail blocks hold the 0x80 marker and the 64-bit length.
struct PadShape {
    std::size_t length = 0;
    std::size_t full = 0;
    std::size_t remainder = 0;
    std::size_t tailBlocks = 1;
};

[[nodiscard]] PadShape ShapeOf(std::size_t length) noexcept {
    const std::size_t remainder = length % kBlockBytes;
    return PadShape{length, length / kBlockBytes, remainder, remainder + 9 <= kBlockBytes ? std::size_t{1} : std::size_t{2}};
}

// One message in the current batch: its input, its padded tail, and how many of each.
// PrepareLane writes every field that is later read, so the array of lanes does not need to be zero-initialised up front.
struct Lane {
    std::span<const std::byte> input;
    std::array<std::byte, 2 * kBlockBytes> tail;
    std::size_t fullBlocks;
    std::size_t totalBlocks;
};

// Pad one message independently.
void PrepareLane(Lane& lane, std::span<const std::byte> input) noexcept {
    lane.input = input;

    const PadShape shape = ShapeOf(input.size());

    // Constant sizes keep these as inline vector stores instead of a variable-length memset call.
    if (shape.tailBlocks == 1) {
        std::memset(lane.tail.data(), 0, kBlockBytes);
    } else {
        std::memset(lane.tail.data(), 0, 2 * kBlockBytes);
    }
    if (shape.remainder != 0) {
        CopyPrefix(lane.tail.data(), input.data() + shape.full * kBlockBytes, shape.remainder);
    }
    lane.tail[shape.remainder] = std::byte{0x80};
    StoreLe64(lane.tail.data() + shape.tailBlocks * kBlockBytes - 8, static_cast<std::uint64_t>(shape.length) * 8);

    lane.fullBlocks = shape.full;
    lane.totalBlocks = shape.full + shape.tailBlocks;
}

// Copies one prepared tail into every lane.
// The size is a template argument so that the copy is a fixed number of moves, which is the whole point of sharing the tail instead of padding each message separately.
template <std::size_t TailBlocks>
void FillTails(std::array<Lane, kMaxLanes>& lanes, std::size_t width, const std::array<std::byte, 2 * kBlockBytes>& pad) noexcept {
    for (std::size_t lane = 0; lane < width; ++lane) {
        std::memcpy(lanes[lane].tail.data(), pad.data(), TailBlocks * kBlockBytes);
    }
}

// Everything one md5_batch call needs while it works through the messages.
// One object rather than several separate locals keeps the block pointers and the kernels' state pointer next to each other on the stack.
struct Scratch {
    std::array<Lane, kMaxLanes> lanes;
    std::array<const std::byte*, kMaxLanes> blocks;
    // SoA state: a[lanes], b[lanes], c[lanes], d[lanes].
    std::array<std::uint32_t, 4 * kMaxLanes> state;
    std::array<std::byte, kBlockBytes> zero{};

    // Four constant fills instead of four stores per lane: the initial state is the same for every lane, so the SoA layout lets one vector store cover sixteen lanes at a time.
    void ResetState(std::size_t width) noexcept {
        for (std::size_t role = 0; role < 4; ++role) {
            std::fill_n(state.data() + role * width, width, kInitialState[role]);
        }
    }

    void StoreDigests(std::span<Digest> digests, std::size_t first, std::size_t count, std::size_t width) const noexcept {
        for (std::size_t lane = 0; lane < count; ++lane) {
            digests[first + lane] = utilities::PackValues(state[0 * width + lane], state[1 * width + lane], state[2 * width + lane], state[3 * width + lane]);
        }
    }

    // Hashes messages that already carry their padding, one whole 64-byte block per message and no tail to build.
    // The caller guarantees that every input has the same size, so one block count and one digest write serve the call.
    HASHLIB_BATCH_INLINE void HashPadded(std::span<const std::span<const std::byte>> inputs, std::span<Digest> digests, std::size_t first, std::size_t count, std::size_t width, const detail::BatchKernel& kernel) noexcept {
        HashPaddedState(inputs, first, count, width, kernel);
        StoreDigests(digests, first, count, width);
    }

    // The block loop both padded entry points share, leaving the digests in `state`.
    HASHLIB_BATCH_INLINE void HashPaddedState(std::span<const std::span<const std::byte>> inputs, std::size_t first, std::size_t count, std::size_t width, const detail::BatchKernel& kernel) noexcept {
        const std::size_t blockCount = inputs[first].size() / kBlockBytes;

        ResetState(width);

        for (std::size_t block = 0; block < blockCount; ++block) {
            const std::size_t offset = block * kBlockBytes;
            if (count == width) {
                for (std::size_t lane = 0; lane < width; ++lane) {
                    blocks[lane] = inputs[first + lane].data() + offset;
                }
            } else {
                for (std::size_t lane = 0; lane < width; ++lane) {
                    blocks[lane] = lane < count ? inputs[first + lane].data() + offset : zero.data();
                }
            }
            kernel.compress(blocks.data(), state.data());
        }
    }

    // The index of the first lane whose digest is `wanted`, or `kNoDigestMatch`.
    // Almost every lane fails on the first state word, so the other three are only read when they can matter; materialising the digests first would write four times as much and read it back.
    [[nodiscard]] std::size_t FindDigest(const std::array<std::uint32_t, 4>& wanted, std::size_t first, std::size_t count, std::size_t width) const noexcept {
        for (std::size_t lane = 0; lane < count; ++lane) {
            if (state[0 * width + lane] != wanted[0]) {
                continue;
            }
            if (state[1 * width + lane] != wanted[1] || state[2 * width + lane] != wanted[2] || state[3 * width + lane] != wanted[3]) {
                continue;
            }
            return first + lane;
        }
        return kNoDigestMatch;
    }

    // Hashes a batch whose messages all have the same length.
    // The padding is then identical for every lane, so one tail block is built once and copied, and every lane finishes on the same block, which removes both the per-lane block choice and the per-lane digest test.
    // The last batch of a call may be short; the lanes it leaves over get the same tail and are hashed as zeros, which is well defined and discarded.
    HASHLIB_BATCH_INLINE void HashUniform(std::span<const std::span<const std::byte>> inputs, std::span<Digest> digests, std::size_t first, std::size_t count, std::size_t width, const PadShape& shape, const detail::BatchKernel& kernel) noexcept {
        std::array<std::byte, 2 * kBlockBytes> pad{};
        pad[shape.remainder] = std::byte{0x80};
        StoreLe64(pad.data() + shape.tailBlocks * kBlockBytes - 8, static_cast<std::uint64_t>(shape.length) * 8);

        if (shape.tailBlocks == 1) {
            FillTails<1>(lanes, width, pad);
        } else {
            FillTails<2>(lanes, width, pad);
        }
        if (shape.remainder != 0) {
            for (std::size_t lane = 0; lane < count; ++lane) {
                CopyPrefix(lanes[lane].tail.data(), inputs[first + lane].data() + shape.full * kBlockBytes, shape.remainder);
            }
        }

        ResetState(width);

        const std::size_t totalBlocks = shape.full + shape.tailBlocks;
        for (std::size_t block = 0; block < totalBlocks; ++block) {
            if (block < shape.full) {
                for (std::size_t lane = 0; lane < width; ++lane) {
                    blocks[lane] = lane < count ? inputs[first + lane].data() + block * kBlockBytes : zero.data();
                }
            } else {
                const std::size_t offset = (block - shape.full) * kBlockBytes;
                for (std::size_t lane = 0; lane < width; ++lane) {
                    blocks[lane] = lanes[lane].tail.data() + offset;
                }
            }
            kernel.compress(blocks.data(), state.data());
        }

        StoreDigests(digests, first, count, width);
    }

    // Hashes a batch of messages that may differ in length.
    // Lanes whose message is done are fed zero blocks so the shorter lanes can keep the batch full, which changes their chaining value, so each lane's digest is captured immediately after its own last block rather than after the loop.
    // Lanes never interact, so a shorter lane's extra zero blocks cannot disturb the lanes that are still running.
    HASHLIB_BATCH_INLINE void HashMixed(std::span<const std::span<const std::byte>> inputs, std::span<Digest> digests, std::size_t first, std::size_t count, std::size_t width, const detail::BatchKernel& kernel) noexcept {
        std::size_t maxBlocks = 0;
        for (std::size_t lane = 0; lane < width; ++lane) {
            PrepareLane(lanes[lane], lane < count ? inputs[first + lane] : std::span<const std::byte>{});
            maxBlocks = std::max(maxBlocks, lanes[lane].totalBlocks);
        }

        ResetState(width);

        for (std::size_t block = 0; block < maxBlocks; ++block) {
            for (std::size_t lane = 0; lane < width; ++lane) {
                const Lane& current = lanes[lane];
                if (block < current.fullBlocks) {
                    blocks[lane] = current.input.data() + block * kBlockBytes;
                } else if (block < current.totalBlocks) {
                    blocks[lane] = current.tail.data() + (block - current.fullBlocks) * kBlockBytes;
                } else {
                    blocks[lane] = zero.data();
                }
            }

            kernel.compress(blocks.data(), state.data());

            for (std::size_t lane = 0; lane < count; ++lane) {
                if (block + 1 == lanes[lane].totalBlocks) {
                    digests[first + lane] = utilities::PackValues(state[0 * width + lane], state[1 * width + lane], state[2 * width + lane], state[3 * width + lane]);
                }
            }
        }
    }
};

#undef HASHLIB_BATCH_INLINE

// The dispatcher does the CPU detection once; this wrapper caches the choice.
// A function-local static costs one guard check per md5_batch call (never per block), and is safe if a caller reaches the library during static init.
const detail::KernelSet& SelectedKernels() noexcept {
    static const detail::KernelSet kernels = detail::SelectBatchKernels();
    return kernels;
}

// The narrowest kernel that still covers the batch.
// One call costs the same as one on a narrower kernel, so covering more messages per call is free; a batch larger than the widest kernel simply takes more calls.
const detail::BatchKernel& SelectedKernel(std::size_t messages) noexcept {
    const detail::KernelSet& set = SelectedKernels();
    for (std::size_t i = 0; i < set.count; ++i) {
        if (set.kernels[i].lanes >= messages) {
            return set.kernels[i];
        }
    }
    return set.kernels[set.count - 1];
}

}  // namespace

BatchInfo md5_batch_info() noexcept {
    const detail::KernelSet& set = SelectedKernels();
    const detail::BatchKernel& widest = set.kernels[set.count - 1];
    return BatchInfo{widest.name, widest.lanes};
}

namespace {

// The whole batch body, sharing every step between the two public entry points.
// The flag is a template parameter so each entry point compiles its own copy with the length check gone rather than carried as a branch through every batch.
template <bool kUniform>
void HashBatches(std::span<const std::span<const std::byte>> inputs, std::span<Digest> digests) noexcept {
    assert(digests.size() >= inputs.size());
    if (digests.size() < inputs.size()) {
        return;
    }
    if (inputs.empty()) {
        return;
    }

    const detail::BatchKernel& kernel = SelectedKernel(inputs.size());

    // No SIMD kernel (non-x86 target, HASHLIB_SIMD=OFF, or a forced scalar run), or only one message: batching would add nothing, so use the single-message path.
    if (kernel.compress == nullptr || inputs.size() == 1) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            digests[i] = md5_fast(inputs[i]);
        }
        return;
    }

    const std::size_t width = kernel.lanes;
    Scratch scratch;

    for (std::size_t first = 0; first < inputs.size(); first += width) {
        const std::size_t count = std::min(width, inputs.size() - first);
        const std::size_t length = inputs[first].size();

        // Callers that group their messages by length, as the phrase solver does, get the uniform path for every batch; mixed input pays one compare per message for the check.
        if constexpr (kUniform) {
            scratch.HashUniform(inputs, digests, first, count, width, ShapeOf(length), kernel);
        } else {
            bool sameLength = true;
            for (std::size_t lane = 1; lane < count; ++lane) {
                sameLength = sameLength && inputs[first + lane].size() == length;
            }
            if (sameLength) {
                scratch.HashUniform(inputs, digests, first, count, width, ShapeOf(length), kernel);
            } else {
                scratch.HashMixed(inputs, digests, first, count, width, kernel);
            }
        }
    }
}

}  // namespace

void md5_batch(std::span<const std::span<const std::byte>> inputs, std::span<Digest> digests) noexcept {
    HashBatches<false>(inputs, digests);
}

void md5_batch_uniform(std::span<const std::span<const std::byte>> inputs, std::span<Digest> digests) noexcept {
#ifndef NDEBUG
    for (const std::span<const std::byte>& input : inputs) {
        assert(input.size() == inputs.front().size());
    }
#endif
    HashBatches<true>(inputs, digests);
}

// The padded entry points share one contract: every input is a whole number of blocks and every input has the same size.
// A debug build checks both, a release build trusts the caller.
void AssertPaddedBlocks(std::span<const std::span<const std::byte>> blocks) noexcept {
    assert(!blocks.front().empty() && blocks.front().size() % kBlockBytes == 0);
#ifndef NDEBUG
    for (const std::span<const std::byte>& block : blocks) {
        assert(block.size() == blocks.front().size());
    }
#endif
}

// The four state words a digest stands for, in the little-endian order MD5 defines.
[[nodiscard]] std::array<std::uint32_t, 4> DigestWords(const Digest& digest) noexcept {
    return {LoadLe32(digest.data()), LoadLe32(digest.data() + 4), LoadLe32(digest.data() + 8), LoadLe32(digest.data() + 12)};
}

// Hashes one pre-padded message with the scalar core and returns its state.
[[nodiscard]] std::array<std::uint32_t, 4> HashPaddedScalar(std::span<const std::byte> block) noexcept {
    std::array<std::uint32_t, 4> state{kInitialState[0], kInitialState[1], kInitialState[2], kInitialState[3]};
    detail::CompressPaddedScalar(block.data(), block.size() / kBlockBytes, state.data());
    return state;
}

void md5_batch_padded(std::span<const std::span<const std::byte>> blocks, std::span<Digest> digests) noexcept {
    assert(digests.size() >= blocks.size());
    if (digests.size() < blocks.size()) {
        return;
    }
    if (blocks.empty()) {
        return;
    }
    AssertPaddedBlocks(blocks);

    const detail::BatchKernel& kernel = SelectedKernel(blocks.size());

    // No SIMD kernel (non-x86 target, HASHLIB_SIMD=OFF, or a forced scalar run): the blocks are already padded, so md5_fast, which pads, cannot be used here.
    if (kernel.compress == nullptr) {
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const std::array<std::uint32_t, 4> state = HashPaddedScalar(blocks[i]);
            digests[i] = utilities::PackValues(state[0], state[1], state[2], state[3]);
        }
        return;
    }

    const std::size_t width = kernel.lanes;
    Scratch scratch;

    for (std::size_t first = 0; first < blocks.size(); first += width) {
        const std::size_t count = std::min(width, blocks.size() - first);
        scratch.HashPadded(blocks, digests, first, count, width, kernel);
    }
}

std::size_t md5_batch_padded_find(std::span<const std::span<const std::byte>> blocks, const Digest& wanted) noexcept {
    if (blocks.empty()) {
        return kNoDigestMatch;
    }
    AssertPaddedBlocks(blocks);

    const std::array<std::uint32_t, 4> words = DigestWords(wanted);
    const detail::BatchKernel& kernel = SelectedKernel(blocks.size());

    if (kernel.compress == nullptr) {
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (HashPaddedScalar(blocks[i]) == words) {
                return i;
            }
        }
        return kNoDigestMatch;
    }

    const std::size_t width = kernel.lanes;
    Scratch scratch;

    for (std::size_t first = 0; first < blocks.size(); first += width) {
        const std::size_t count = std::min(width, blocks.size() - first);
        scratch.HashPaddedState(blocks, first, count, width, kernel);
        const std::size_t found = scratch.FindDigest(words, first, count, width);
        if (found != kNoDigestMatch) {
            return found;
        }
    }
    return kNoDigestMatch;
}

}  // namespace hashlib
