#include "minidb/record.hpp"

#include <string>

#include "minidb/diagnostic.hpp"
#include "minidb/page.hpp"

namespace minidb {
namespace {

constexpr std::uint8_t kTagNull = 0;
constexpr std::uint8_t kTagInteger = 1;
constexpr std::uint8_t kTagReal = 2;
constexpr std::uint8_t kTagText = 3;

[[noreturn]] void corrupt(const std::string& what) {
    throw DbError("corrupt record: " + what);
}

}  // namespace

std::size_t encoded_size(const Row& row) {
    std::size_t total = 0;
    for (const Value& v : row) {
        total += 1;  // type tag
        switch (v.type()) {
            case ValueType::Null: break;
            case ValueType::Integer:
            case ValueType::Real: total += 8; break;
            case ValueType::Text: total += 4 + v.as_text().size(); break;
        }
    }
    return total;
}

void encode_row(const Row& row, std::vector<std::uint8_t>& out) {
    out.clear();
    out.resize(encoded_size(row));
    std::size_t at = 0;
    for (const Value& v : row) {
        switch (v.type()) {
            case ValueType::Null:
                out[at++] = kTagNull;
                break;
            case ValueType::Integer:
                out[at++] = kTagInteger;
                put_i64(out.data() + at, v.as_integer());
                at += 8;
                break;
            case ValueType::Real:
                out[at++] = kTagReal;
                put_f64(out.data() + at, v.as_double());
                at += 8;
                break;
            case ValueType::Text: {
                out[at++] = kTagText;
                const std::string& text = v.as_text();
                put_u32(out.data() + at, static_cast<std::uint32_t>(text.size()));
                at += 4;
                if (!text.empty()) {
                    std::memcpy(out.data() + at, text.data(), text.size());
                    at += text.size();
                }
                break;
            }
        }
    }
}

std::vector<std::uint8_t> encode_row(const Row& row) {
    std::vector<std::uint8_t> out;
    encode_row(row, out);
    return out;
}

Row decode_row(const std::uint8_t* data, std::size_t size) {
    Row row;
    std::size_t at = 0;
    while (at < size) {
        const std::uint8_t tag = data[at++];
        switch (tag) {
            case kTagNull:
                row.push_back(Value::null());
                break;
            case kTagInteger:
                if (size - at < 8) corrupt("truncated INTEGER");
                row.push_back(Value::integer(get_i64(data + at)));
                at += 8;
                break;
            case kTagReal:
                if (size - at < 8) corrupt("truncated REAL");
                row.push_back(Value::real(get_f64(data + at)));
                at += 8;
                break;
            case kTagText: {
                if (size - at < 4) corrupt("truncated TEXT length");
                const std::size_t len = get_u32(data + at);
                at += 4;
                if (size - at < len) corrupt("truncated TEXT payload");
                row.push_back(Value::text(
                    std::string(reinterpret_cast<const char*>(data + at), len)));
                at += len;
                break;
            }
            default:
                corrupt("unknown type tag " + std::to_string(static_cast<int>(tag)));
        }
    }
    return row;
}

}  // namespace minidb
