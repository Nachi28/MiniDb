#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <variant>

namespace minidb {

enum class ValueType { Null, Integer, Real, Text };

const char* type_name(ValueType t);

// A single SQL scalar. NULL is a distinct state, not a sentinel.
class Value {
public:
    Value() = default;

    static Value null() { return Value(); }
    static Value integer(std::int64_t v);
    static Value real(double v);
    static Value text(std::string v);
    static Value boolean(bool b);  // stored as INTEGER 1 / 0, like SQLite

    ValueType type() const;
    bool is_null() const;
    bool is_number() const;

    std::int64_t as_integer() const;    // Integer or Real (truncating)
    double as_double() const;           // Integer or Real
    const std::string& as_text() const; // Text only

    std::string to_string() const;  // display form: NULL -> "NULL", text unquoted
    std::string to_sql() const;     // literal form: text single-quoted and escaped

private:
    std::variant<std::monostate, std::int64_t, double, std::string> data_{};
};

bool operator==(const Value& a, const Value& b);
bool operator!=(const Value& a, const Value& b);
std::ostream& operator<<(std::ostream& os, const Value& v);

// SQL three-valued ordering. Returns nullopt when either operand is NULL,
// which is what makes `WHERE x = NULL` correctly match nothing.
// Ordering across types follows SQLite: numbers sort before text.
std::optional<int> compare(const Value& a, const Value& b);

}  // namespace minidb
