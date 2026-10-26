export module hashlib.utilities;

import std;

/// General-purpose helpers shared by the hash library.
///
/// Intentionally empty for now; static utility functions will live here.
export namespace hashlib::utilities {

    [[nodiscard]] std::array<std::byte, 16> PackValues(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d);

}  // namespace hashlib::utilities
