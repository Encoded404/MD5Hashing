module;

module hashlib.utilities;

import std;

struct FourWords {
    std::uint32_t a, b, c, d;
};

static_assert(sizeof(FourWords) == 16);

namespace hashlib::utilities {

    std::array<std::byte, 16> PackValues(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        return std::bit_cast<std::array<std::byte, 16>>(FourWords{a, b, c, d});
    }


}  // namespace hashlib::utilities
