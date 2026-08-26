#include "minidb/record.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "minidb/diagnostic.hpp"
#include "test_framework.hpp"

using namespace minidb;

namespace {

Row round_trip(const Row& row) {
    const std::vector<std::uint8_t> bytes = encode_row(row);
    REQUIRE_EQ(bytes.size(), encoded_size(row));
    return decode_row(bytes.data(), bytes.size());
}

}  // namespace

TEST(Record, RoundTripsEveryType) {
    const Row row = {Value::integer(-42), Value::real(3.5), Value::text("hello"),
                     Value::null()};
    const Row back = round_trip(row);
    REQUIRE_EQ(back.size(), static_cast<std::size_t>(4));
    REQUIRE_EQ(back[0], Value::integer(-42));
    REQUIRE_EQ(back[1], Value::real(3.5));
    REQUIRE_EQ(back[2], Value::text("hello"));
    REQUIRE(back[3].is_null());
}

TEST(Record, RoundTripsAnEmptyRow) {
    REQUIRE_EQ(round_trip(Row{}).size(), static_cast<std::size_t>(0));
    REQUIRE_EQ(encoded_size(Row{}), static_cast<std::size_t>(0));
}

TEST(Record, RoundTripsEmptyAndEmbeddedNullText) {
    const Row row = {Value::text(""), Value::text(std::string("a\0b", 3))};
    const Row back = round_trip(row);
    REQUIRE_EQ(back[0], Value::text(""));
    REQUIRE_EQ(back[1].as_text().size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(back[1], Value::text(std::string("a\0b", 3)));
}

TEST(Record, RoundTripsExtremeNumbers) {
    const std::int64_t lo = -9223372036854775807LL - 1;
    const std::int64_t hi = 9223372036854775807LL;
    const Row row = {Value::integer(lo), Value::integer(hi), Value::real(-0.0),
                     Value::real(1e308)};
    const Row back = round_trip(row);
    REQUIRE_EQ(back[0], Value::integer(lo));
    REQUIRE_EQ(back[1], Value::integer(hi));
    REQUIRE_EQ(back[2], Value::real(-0.0));
    REQUIRE_EQ(back[3], Value::real(1e308));
}

TEST(Record, SizesAreExactlyWhatTheEncodingPromises) {
    REQUIRE_EQ(encoded_size(Row{Value::null()}), static_cast<std::size_t>(1));
    REQUIRE_EQ(encoded_size(Row{Value::integer(1)}), static_cast<std::size_t>(9));
    REQUIRE_EQ(encoded_size(Row{Value::real(1.0)}), static_cast<std::size_t>(9));
    REQUIRE_EQ(encoded_size(Row{Value::text("abc")}), static_cast<std::size_t>(8));
}

TEST(Record, EncodingIsIndependentOfHostByteOrder) {
    const std::vector<std::uint8_t> bytes = encode_row(Row{Value::integer(1)});
    REQUIRE_EQ(bytes[0], static_cast<std::uint8_t>(1));  // INTEGER tag
    REQUIRE_EQ(bytes[1], static_cast<std::uint8_t>(1));  // little-endian low byte
    REQUIRE_EQ(bytes[8], static_cast<std::uint8_t>(0));
}

TEST(Record, RejectsTruncatedRecords) {
    const std::vector<std::uint8_t> bytes = encode_row(Row{Value::integer(7)});
    REQUIRE_THROWS_AS(decode_row(bytes.data(), bytes.size() - 1), DbError);

    const std::vector<std::uint8_t> text = encode_row(Row{Value::text("abcdef")});
    REQUIRE_THROWS_AS(decode_row(text.data(), text.size() - 2), DbError);
    REQUIRE_THROWS_AS(decode_row(text.data(), 3), DbError);
}

TEST(Record, RejectsAnUnknownTypeTag) {
    const std::vector<std::uint8_t> bytes = {99};
    REQUIRE_THROWS_AS(decode_row(bytes.data(), bytes.size()), DbError);
}
