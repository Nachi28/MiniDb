#include "minidb/key.hpp"

#include <cstring>

#include "minidb/diagnostic.hpp"

namespace minidb {
namespace {

constexpr std::uint8_t kTagNull = 0x00;
constexpr std::uint8_t kTagNumber = 0x01;
constexpr std::uint8_t kTagText = 0x02;

void append_be64(std::string& out, std::uint64_t bits) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((bits >> shift) & 0xFFu));
    }
}

std::uint64_t read_be64(const std::uint8_t* data) {
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | data[i];
    return bits;
}

// IEEE-754 doubles compare correctly as big-endian integers once the sign is
// folded in: flip every bit of a negative, set the top bit of a positive.
std::uint64_t order_preserving(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if ((bits & (1ULL << 63)) != 0) return ~bits;
    return bits | (1ULL << 63);
}

std::uint64_t order_preserving(std::int64_t value) {
    // Sign-flipped two's complement keeps integer order exactly, with no
    // rounding, which matters because rowids are integers.
    return static_cast<std::uint64_t>(value) ^ (1ULL << 63);
}

}  // namespace

std::string encode_rowid(std::int64_t rowid) {
    std::string out;
    out.reserve(8);
    append_be64(out, order_preserving(rowid));
    return out;
}

std::int64_t decode_rowid(Slice key) {
    if (key.size < 8) throw DbError("corrupt rowid key");
    return static_cast<std::int64_t>(read_be64(key.data) ^ (1ULL << 63));
}

std::string encode_value(const Value& value) {
    std::string out;
    switch (value.type()) {
        case ValueType::Null:
            out.push_back(static_cast<char>(kTagNull));
            return out;

        case ValueType::Integer:
        case ValueType::Real:
            out.push_back(static_cast<char>(kTagNumber));
            append_be64(out, order_preserving(value.as_double()));
            return out;

        case ValueType::Text: {
            out.push_back(static_cast<char>(kTagText));
            // 0x00 is escaped so it cannot be confused with the terminator, and
            // 0x00 0xFF still sorts before 0x00 0x00, preserving order.
            for (char c : value.as_text()) {
                out.push_back(c);
                if (c == '\0') out.push_back(static_cast<char>(0xFF));
            }
            out.push_back('\0');
            out.push_back('\0');
            return out;
        }
    }
    return out;
}

std::string encode_index_key(const Value& value, std::int64_t rowid) {
    return encode_value(value) + encode_rowid(rowid);
}

std::string encode_index_upper_bound(const Value& value) {
    // Every entry for `value` is encode_value(value) followed by exactly 8 rowid
    // bytes. Nine 0xFF bytes beat all of them: it ties the largest possible
    // rowid (encode_rowid(INT64_MAX) is all 0xFF) over the first eight bytes and
    // then wins on length. It still loses to the next value, which differs
    // earlier, inside encode_value.
    std::string out = encode_value(value);
    out.append(9, static_cast<char>(0xFF));
    return out;
}

std::int64_t index_key_rowid(Slice key) {
    if (key.size < 8) throw DbError("corrupt index key");
    return decode_rowid(Slice(key.data + key.size - 8, 8));
}

}  // namespace minidb
