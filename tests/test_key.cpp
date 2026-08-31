#include "minidb/key.hpp"

#include <string>
#include <vector>

#include "test_framework.hpp"

using namespace minidb;

namespace {

int order(const std::string& a, const std::string& b) {
    return compare_slices(Slice(a), Slice(b));
}

// The whole point of the encoding: byte order must equal SQL order.
void assert_ascending(const std::vector<Value>& values) {
    for (std::size_t i = 1; i < values.size(); ++i) {
        const std::string lo = encode_value(values[i - 1]);
        const std::string hi = encode_value(values[i]);
        REQUIRE(order(lo, hi) < 0);
    }
}

}  // namespace

TEST(Key, RowidsRoundTrip) {
    const std::int64_t cases[] = {0, 1, -1, 42, -9999,
                                  9223372036854775807LL,
                                  -9223372036854775807LL - 1};
    for (std::int64_t rowid : cases) {
        const std::string encoded = encode_rowid(rowid);
        REQUIRE_EQ(encoded.size(), static_cast<std::size_t>(8));
        REQUIRE_EQ(decode_rowid(Slice(encoded)), rowid);
    }
}

TEST(Key, RowidOrderMatchesIntegerOrder) {
    const std::int64_t ordered[] = {-9223372036854775807LL - 1, -1000, -1, 0, 1,
                                    1000, 9223372036854775807LL};
    for (std::size_t i = 1; i < sizeof(ordered) / sizeof(ordered[0]); ++i) {
        REQUIRE(order(encode_rowid(ordered[i - 1]), encode_rowid(ordered[i])) < 0);
    }
}

TEST(Key, StorageClassesSortNullThenNumbersThenText) {
    assert_ascending({Value::null(), Value::integer(-5), Value::integer(0),
                      Value::real(0.5), Value::integer(1), Value::text(""),
                      Value::text("a")});
}

TEST(Key, NumbersSortNumerically) {
    assert_ascending({Value::real(-1e308), Value::integer(-1000), Value::real(-2.5),
                      Value::integer(-1), Value::real(-0.0), Value::integer(0),
                      Value::real(0.25), Value::integer(1), Value::real(2.5),
                      Value::integer(1000), Value::real(1e308)});
}

TEST(Key, IntegersAndRealsOfEqualValueEncodeIdentically) {
    REQUIRE_EQ(encode_value(Value::integer(3)), encode_value(Value::real(3.0)));
}

TEST(Key, TextSortsLexicographically) {
    assert_ascending({Value::text(""), Value::text("a"), Value::text("aa"),
                      Value::text("ab"), Value::text("b"), Value::text("z")});
}

TEST(Key, EmbeddedNullBytesAreEscapedAndStillOrder) {
    const Value shorter = Value::text(std::string("a\0", 2));
    const Value longer = Value::text(std::string("a\0b", 3));
    const Value plain = Value::text("ab");
    REQUIRE(order(encode_value(shorter), encode_value(longer)) < 0);
    REQUIRE(order(encode_value(longer), encode_value(plain)) < 0);
    REQUIRE(order(encode_value(Value::text("a")), encode_value(shorter)) < 0);
}

TEST(Key, IndexKeysCarryTheirRowid) {
    const std::string key = encode_index_key(Value::text("hello"), 12345);
    REQUIRE_EQ(index_key_rowid(Slice(key)), static_cast<std::int64_t>(12345));
    REQUIRE_EQ(key.size(), encode_value(Value::text("hello")).size() + 8);
}

TEST(Key, EqualValuesSortByRowid) {
    const std::string first = encode_index_key(Value::integer(7), 1);
    const std::string second = encode_index_key(Value::integer(7), 2);
    REQUIRE(order(first, second) < 0);
}

TEST(Key, TheUpperBoundSitsJustPastEveryEntryForAValue) {
    const std::string bound = encode_index_upper_bound(Value::integer(7));
    // Including INT64_MAX, whose encoded rowid is all 0xFF -- the case that
    // makes an 8-byte bound tie instead of exceeding.
    const std::int64_t rowids[] = {0, 1, 999999, 9223372036854775807LL};
    for (std::int64_t rowid : rowids) {
        REQUIRE(order(encode_index_key(Value::integer(7), rowid), bound) < 0);
    }
    // ...and before anything belonging to the next value.
    REQUIRE(order(bound, encode_index_key(Value::integer(8), 0)) < 0);
    REQUIRE(order(encode_index_key(Value::integer(6), 9999), bound) < 0);
}
