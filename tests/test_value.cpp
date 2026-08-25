#include "minidb/value.hpp"

#include "test_framework.hpp"

using namespace minidb;

TEST(Value, DefaultIsNull) {
    const Value v;
    REQUIRE(v.is_null());
    REQUIRE(v.type() == ValueType::Null);
    REQUIRE_EQ(v.to_string(), std::string("NULL"));
}

TEST(Value, IntegerRoundTrip) {
    const Value v = Value::integer(-42);
    REQUIRE(v.type() == ValueType::Integer);
    REQUIRE(v.is_number());
    REQUIRE_EQ(v.as_integer(), static_cast<std::int64_t>(-42));
    REQUIRE_EQ(v.to_string(), std::string("-42"));
}

TEST(Value, TextIsQuotedAndEscapedInSqlForm) {
    const Value v = Value::text("O'Brien");
    REQUIRE_EQ(v.as_text(), std::string("O'Brien"));
    REQUIRE_EQ(v.to_string(), std::string("O'Brien"));
    REQUIRE_EQ(v.to_sql(), std::string("'O''Brien'"));
}

TEST(Value, BooleanIsStoredAsInteger) {
    REQUIRE_EQ(Value::boolean(true).as_integer(), static_cast<std::int64_t>(1));
    REQUIRE_EQ(Value::boolean(false).as_integer(), static_cast<std::int64_t>(0));
}

TEST(Value, EqualityTreatsNullAsIdentical) {
    REQUIRE(Value::null() == Value::null());
    REQUIRE(Value::null() != Value::integer(0));
}

TEST(Value, EqualityComparesAcrossNumericTypes) {
    REQUIRE(Value::integer(3) == Value::real(3.0));
    REQUIRE(Value::integer(3) != Value::text("3"));
}

TEST(Value, CompareReturnsNulloptWhenEitherSideIsNull) {
    REQUIRE(!compare(Value::null(), Value::integer(1)).has_value());
    REQUIRE(!compare(Value::integer(1), Value::null()).has_value());
    REQUIRE(!compare(Value::null(), Value::null()).has_value());
}

TEST(Value, CompareOrdersNumbers) {
    REQUIRE_EQ(compare(Value::integer(1), Value::integer(2)).value(), -1);
    REQUIRE_EQ(compare(Value::real(2.5), Value::integer(2)).value(), 1);
    REQUIRE_EQ(compare(Value::integer(7), Value::real(7.0)).value(), 0);
}

TEST(Value, CompareOrdersTextLexicographically) {
    REQUIRE_EQ(compare(Value::text("apple"), Value::text("banana")).value(), -1);
    REQUIRE_EQ(compare(Value::text("pear"), Value::text("pear")).value(), 0);
}

TEST(Value, CompareSortsNumbersBeforeText) {
    REQUIRE_EQ(compare(Value::integer(9), Value::text("1")).value(), -1);
    REQUIRE_EQ(compare(Value::text("1"), Value::integer(9)).value(), 1);
}

TEST(Value, AccessorsRejectWrongType) {
    REQUIRE_THROWS_AS(Value::text("x").as_double(), std::logic_error);
    REQUIRE_THROWS_AS(Value::integer(1).as_text(), std::logic_error);
    REQUIRE_THROWS_AS(Value::null().as_integer(), std::logic_error);
}
