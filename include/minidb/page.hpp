#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace minidb {

using PageId = std::uint32_t;

inline constexpr std::size_t kPageSize = 4096;

// Page 0 holds the file header, so it is never a valid node page and doubles
// as the "no such page" sentinel.
inline constexpr PageId kInvalidPage = 0;

// Everything on disk is little-endian regardless of host byte order, so a
// database file written on one machine reads correctly on another.

inline void put_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

inline std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(p[0]) |
                                      (static_cast<unsigned>(p[1]) << 8));
}

inline void put_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
}

inline std::uint32_t get_u32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

inline void put_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
}

inline std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

inline void put_i64(std::uint8_t* p, std::int64_t v) {
    put_u64(p, static_cast<std::uint64_t>(v));
}

inline std::int64_t get_i64(const std::uint8_t* p) {
    return static_cast<std::int64_t>(get_u64(p));
}

inline void put_f64(std::uint8_t* p, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u64(p, bits);
}

inline double get_f64(const std::uint8_t* p) {
    const std::uint64_t bits = get_u64(p);
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

}  // namespace minidb
