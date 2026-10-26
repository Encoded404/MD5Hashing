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
/// The MD5 core is not implemented yet; until it lands this function throws
/// `std::logic_error`. The declaration below is the intended call surface.
[[nodiscard]] Digest naive_md5(std::span<const std::byte> input);

}  // namespace hashlib
