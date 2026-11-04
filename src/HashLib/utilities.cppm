export module hashlib.utilities;

import std;

/// General-purpose helpers shared by the hash library.
export namespace hashlib::utilities {

    /// Packs the four MD5 state words into the raw digest layout, in order, without byte swapping.
    /// Defined in the interface rather than in an implementation unit so that every caller inlines it: it is four moves, and an out-of-line call costs more than the work.
    [[nodiscard]] inline std::array<std::byte, 16> PackValues(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        const std::array<std::uint32_t, 4> words{a, b, c, d};
        return std::bit_cast<std::array<std::byte, 16>>(words);
    }

}  // namespace hashlib::utilities
