// The many-message entry point. This is an implementation unit of `hashlib`:
// md5_fast and the public types come from the module interface, while the
// compression kernels come from the internal hashlib.batch module.

module;

#include <cassert>

module hashlib;

import std;
import hashlib.utilities;
import hashlib.batch;

namespace hashlib {
namespace {

constexpr std::size_t kBlockBytes = 64;
constexpr std::size_t kMaxLanes = 16;

void StoreLe64(std::byte* bytes, std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    std::memcpy(bytes, &value, sizeof(value));
}

// One message in the current batch: its input, its padded tail, and where the
// blocks are. The tail always holds the 0x80 marker and the 64-bit length.
// PrepareLane writes every field that is later read, so the array of lanes does
// not need to be zero-initialised up front.
struct Lane {
    std::span<const std::byte> input;
    std::array<std::byte, 2 * kBlockBytes> tail;
    std::size_t fullBlocks;
    std::size_t totalBlocks;
};

// Pad one message independently. The padded data is one block when the marker
// and length fit, two otherwise; full blocks are hashed straight from `input`.
void PrepareLane(Lane& lane, std::span<const std::byte> input) noexcept {
    lane.input = input;

    const std::size_t length = input.size();
    const std::size_t full = length / kBlockBytes;
    const std::size_t remainder = length % kBlockBytes;
    const std::size_t tailBlocks = (remainder + 9 <= kBlockBytes) ? 1 : 2;

    // Constant sizes keep these as inline vector stores instead of a
    // variable-length memset call.
    if (tailBlocks == 1) {
        std::memset(lane.tail.data(), 0, kBlockBytes);
    } else {
        std::memset(lane.tail.data(), 0, 2 * kBlockBytes);
    }
    if (remainder != 0) {
        std::memcpy(lane.tail.data(), input.data() + full * kBlockBytes, remainder);
    }
    lane.tail[remainder] = std::byte{0x80};
    StoreLe64(lane.tail.data() + tailBlocks * kBlockBytes - 8,
              static_cast<std::uint64_t>(length) * 8);

    lane.fullBlocks = full;
    lane.totalBlocks = full + tailBlocks;
}

// The dispatcher does the CPU detection once; this wrapper caches the choice.
// A function-local static costs one guard check per md5_batch call (never per
// block), and is safe if a caller reaches the library during static init.
const detail::BatchKernel& SelectedKernel() noexcept {
    static const detail::BatchKernel kernel = detail::SelectBatchKernel();
    return kernel;
}

}  // namespace

BatchInfo md5_batch_info() noexcept {
    const detail::BatchKernel& kernel = SelectedKernel();
    return BatchInfo{kernel.name, kernel.lanes};
}

void md5_batch(std::span<const std::span<const std::byte>> inputs,
               std::span<Digest> digests) noexcept {
    assert(digests.size() >= inputs.size());
    if (digests.size() < inputs.size()) {
        return;
    }
    if (inputs.empty()) {
        return;
    }

    const detail::BatchKernel& kernel = SelectedKernel();

    // No SIMD kernel (non-x86 target, HASHLIB_SIMD=OFF, or a forced scalar
    // run), or only one message: batching would add nothing, so use the
    // single-message path.
    if (kernel.compress == nullptr || inputs.size() == 1) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            digests[i] = md5_fast(inputs[i]);
        }
        return;
    }

    const std::size_t lanes = kernel.lanes;
    std::array<Lane, kMaxLanes> laneData;
    std::array<const std::byte*, kMaxLanes> blockPointers;
    // SoA state: a[lanes], b[lanes], c[lanes], d[lanes].
    std::array<std::uint32_t, 4 * kMaxLanes> state;
    const std::array<std::byte, kBlockBytes> zeroBlock{};

    for (std::size_t first = 0; first < inputs.size(); first += lanes) {
        const std::size_t count = std::min(lanes, inputs.size() - first);
        std::size_t maxBlocks = 0;

        for (std::size_t lane = 0; lane < lanes; ++lane) {
            Lane& current = laneData[lane];
            PrepareLane(current, lane < count ? inputs[first + lane]
                                              : std::span<const std::byte>{});
            maxBlocks = std::max(maxBlocks, current.totalBlocks);

            state[0 * lanes + lane] = 0x67452301U;
            state[1 * lanes + lane] = 0xefcdab89U;
            state[2 * lanes + lane] = 0x98badcfeU;
            state[3 * lanes + lane] = 0x10325476U;
        }

        // Lanes whose message is done are fed zero blocks so the shorter lanes
        // can keep the batch full. That changes their chaining value, so each
        // lane's digest is captured immediately after *its* last block, not
        // after the loop. Lanes never interact, so a shorter lane's extra zero
        // blocks cannot disturb the lanes that are still running.
        for (std::size_t block = 0; block < maxBlocks; ++block) {
            for (std::size_t lane = 0; lane < lanes; ++lane) {
                const Lane& current = laneData[lane];
                if (block < current.fullBlocks) {
                    blockPointers[lane] = current.input.data() + block * kBlockBytes;
                } else if (block < current.totalBlocks) {
                    blockPointers[lane] =
                        current.tail.data() + (block - current.fullBlocks) * kBlockBytes;
                } else {
                    blockPointers[lane] = zeroBlock.data();
                }
            }

            kernel.compress(blockPointers.data(), state.data());

            for (std::size_t lane = 0; lane < count; ++lane) {
                if (block + 1 == laneData[lane].totalBlocks) {
                    digests[first + lane] = utilities::PackValues(
                        state[0 * lanes + lane], state[1 * lanes + lane],
                        state[2 * lanes + lane], state[3 * lanes + lane]);
                }
            }
        }
    }
}

}  // namespace hashlib
