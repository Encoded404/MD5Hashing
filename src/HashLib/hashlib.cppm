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

}  // namespace hashlib
