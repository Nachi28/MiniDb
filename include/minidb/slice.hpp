#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace minidb {

// A borrowed view of some bytes. Keys and payloads are passed around as these
// so nothing is copied on the way into or out of a page.
struct Slice {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    Slice() = default;
    Slice(const std::uint8_t* d, std::size_t n) : data(d), size(n) {}

    explicit Slice(const std::string& s)
        : data(reinterpret_cast<const std::uint8_t*>(s.data())), size(s.size()) {}

    bool empty() const { return size == 0; }
    std::string to_string() const {
        return std::string(reinterpret_cast<const char*>(data), size);
    }
};

// Lexicographic byte order, with a shorter prefix sorting first. Key encodings
// are built so that this ordering is the SQL ordering.
inline int compare_slices(Slice a, Slice b) {
    const std::size_t shared = a.size < b.size ? a.size : b.size;
    if (shared > 0) {
        const int cmp = std::memcmp(a.data, b.data, shared);
        if (cmp != 0) return cmp < 0 ? -1 : 1;
    }
    if (a.size == b.size) return 0;
    return a.size < b.size ? -1 : 1;
}

inline bool operator==(Slice a, Slice b) { return compare_slices(a, b) == 0; }
inline bool operator!=(Slice a, Slice b) { return compare_slices(a, b) != 0; }

}  // namespace minidb
