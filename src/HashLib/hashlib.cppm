export module hashlib;

import std;

export namespace hashlib {

/// The raw 16-byte (128-bit) MD5 digest.
using Digest = std::array<std::byte, 16>;

/// Computes the MD5 digest of the raw bytes in `input`.
///
/// Accepts any contiguous byte range; wrap text or binary buffers with
/// `std::as_bytes(std::span{...})` at the call site.
///
/// This is a deliberately naive, single-shot implementation that follows the
/// Wikipedia pseudocode closely: it copies and pads the whole input in memory
/// and favours readability over speed. It is intended for learning and
/// benchmarking, not as a security primitive (MD5 is cryptographically broken)
/// and not as a streaming hash API.
[[nodiscard]] Digest naive_md5(std::span<const std::byte> input);

/// Computes the MD5 digest of the raw bytes in `input`.
///
/// The optimised counterpart of `naive_md5`, with the same result for every input.
/// Full 64-byte blocks are read straight out of the caller's buffer, so nothing is allocated.
/// the function is `noexcept`; only the trailing one or two blocks go through a 128-byte stack buffer.
/// All 64 steps are expanded at compile time, so the round constant, the rotation amount and the message index are constants,
/// and the state is passed by value so it stays in registers for the whole message.
///
/// The speed-up over `naive_md5` depends on the compiler,
/// because `naive_md5`'s rolled loop is exactly the shape that compilers optimise very differently.
/// roughly 1.3-1.5x with Clang, and up to ~3.5x with GCC (whose code for the rolled loop is far worse).
/// `docs/md5-optimization.md` has the measurements behind each choice.
///
/// Not a streaming API.
[[nodiscard]] Digest md5_fast(std::span<const std::byte> input) noexcept;

/// Which batch kernel the runtime selected, and the largest number of
/// independent messages one compression can cover. `isa` is one of
/// "avx512f", "avx2", "sse2" or "scalar". Selection happens once, on first use,
/// from the CPU executing the process, so one binary can use the best
/// instructions available on any x86-64 machine it is run on. Kernels that
/// interleave several independent groups report the total, and `md5_batch`
/// picks whichever of them covers the batch in the fewest calls.
struct BatchInfo {
    std::string_view isa;
    std::size_t lanes;
};

/// Reports the batch kernel that `md5_batch` will use.
[[nodiscard]] BatchInfo md5_batch_info() noexcept;

/// Computes the MD5 digest of every input using the widest SIMD kernel the
/// running CPU supports.
///
/// The inputs are hashed up to `lanes` at a time (see `md5_batch_info()`), with
/// lanes that have no input padded out and their results discarded. Inputs may
/// have different lengths; each lane is padded independently.
///
/// A batch whose messages all have the same length is cheaper, because the padding is then identical for every lane and is built once instead of once per message.
/// Grouping the messages by length when the choice is yours is worth doing.
/// The caller owns the output storage and must provide at least `inputs.size()` entries.
///
/// This is the throughput-oriented entry point: it is fastest when many
/// independent messages are hashed together. For a single message use
/// `md5_fast`, which avoids the batch setup.
void md5_batch(std::span<const std::span<const std::byte>> inputs,
               std::span<Digest> digests) noexcept;

/// Same as `md5_batch`, for a caller that already knows every input has the same size.
///
/// The equality check costs one compare per message, and a caller that groups by length pays it on every batch for a fact it has already established.
/// Declaring that fact here removes the pass; the result is identical to `md5_batch`.
/// A debug build asserts the precondition and a release build trusts it, so passing mixed lengths is a contract violation rather than a slower correct answer.
void md5_batch_uniform(std::span<const std::span<const std::byte>> inputs,
                       std::span<Digest> digests) noexcept;

/// Computes the digest of messages the caller has already padded.
///
/// Each input must hold exactly the bytes MD5 hashes: the message, the 0x80 marker, the zero fill, and the little-endian bit length, so its size is a whole number of 64-byte blocks.
/// Every input must have that same size, which is the shape a caller that groups by length already produces.
/// Nothing is copied and no tail block is built here, so this is the entry point for a caller that can keep its messages padded between batches and would otherwise pay for a fresh tail per message.
/// A debug build asserts both preconditions; a release build trusts them.
void md5_batch_padded(std::span<const std::span<const std::byte>> blocks,
                      std::span<Digest> digests) noexcept;

/// The index `md5_batch_padded_find` returns when no block hashes to the wanted digest.
inline constexpr std::size_t kNoDigestMatch = std::numeric_limits<std::size_t>::max();

/// Hashes `blocks` exactly like `md5_batch_padded` and returns the index of the block whose digest is `wanted`, or `kNoDigestMatch`.
///
/// A caller that hashes in order to compare against one digest does not need the other digests at all.
/// The comparison can be made on the interleaved state the kernel already holds, which is much cheaper than writing every digest out and reading it back to compare.
/// The blocks follow the same contract as `md5_batch_padded`, and the first matching index is returned in batch order.
[[nodiscard]] std::size_t md5_batch_padded_find(std::span<const std::span<const std::byte>> blocks,
                                                const Digest& wanted) noexcept;

}  // namespace hashlib
