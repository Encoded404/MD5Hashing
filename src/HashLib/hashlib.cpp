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
    std::vector<std::byte> buffer(input.size() + (8 + bitsToAppend) / 8);

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

    std::uint16_t blocks = buffer.size() / (512>>3); // get how many 512 bit blocks there are.
    for (std::uint16_t chunk = 0; chunk < blocks; chunk++) {
        std::vector<std::uint32_t> buffers(16);

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

    const Digest zero = utilities::PackValues(A, B, C, D);
    return zero;
}

}  // namespace hashlib
