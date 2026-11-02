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

}  // namespace hashlib
