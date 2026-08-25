#include "minidb/value.hpp"

#include <ostream>
#include <sstream>
#include <stdexcept>

namespace minidb {

const char* type_name(ValueType t) {
    switch (t) {
        case ValueType::Null: return "NULL";
        case ValueType::Integer: return "INTEGER";
        case ValueType::Real: return "REAL";
        case ValueType::Text: return "TEXT";
    }
    return "UNKNOWN";
}

Value Value::integer(std::int64_t v) {
    Value x;
    x.data_ = v;
    return x;
}

Value Value::real(double v) {
    Value x;
    x.data_ = v;
    return x;
}

Value Value::text(std::string v) {
    Value x;
    x.data_ = std::move(v);
    return x;
}

Value Value::boolean(bool b) { return integer(b ? 1 : 0); }

ValueType Value::type() const {
    switch (data_.index()) {
        case 0: return ValueType::Null;
        case 1: return ValueType::Integer;
        case 2: return ValueType::Real;
        default: return ValueType::Text;
    }
}

bool Value::is_null() const { return data_.index() == 0; }

bool Value::is_number() const { return data_.index() == 1 || data_.index() == 2; }

std::int64_t Value::as_integer() const {
    if (data_.index() == 1) return std::get<std::int64_t>(data_);
    if (data_.index() == 2) return static_cast<std::int64_t>(std::get<double>(data_));
    throw std::logic_error("Value::as_integer() on a non-numeric value");
}

double Value::as_double() const {
    if (data_.index() == 1) return static_cast<double>(std::get<std::int64_t>(data_));
    if (data_.index() == 2) return std::get<double>(data_);
    throw std::logic_error("Value::as_double() on a non-numeric value");
}

const std::string& Value::as_text() const {
    if (data_.index() == 3) return std::get<std::string>(data_);
    throw std::logic_error("Value::as_text() on a non-text value");
}

std::string Value::to_string() const {
    switch (data_.index()) {
        case 0:
            return "NULL";
        case 1:
            return std::to_string(std::get<std::int64_t>(data_));
        case 2: {
            std::ostringstream oss;
            oss << std::get<double>(data_);
            return oss.str();
        }
        default:
            return std::get<std::string>(data_);
    }
}

std::string Value::to_sql() const {
    if (data_.index() != 3) return to_string();
    std::string out = "'";
    for (char c : std::get<std::string>(data_)) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

bool operator==(const Value& a, const Value& b) {
    if (a.type() != b.type()) {
        if (a.is_number() && b.is_number()) return a.as_double() == b.as_double();
        return false;
    }
    switch (a.type()) {
        case ValueType::Null: return true;  // NULL IS NULL
        case ValueType::Integer: return a.as_integer() == b.as_integer();
        case ValueType::Real: return a.as_double() == b.as_double();
        case ValueType::Text: return a.as_text() == b.as_text();
    }
    return false;
}

bool operator!=(const Value& a, const Value& b) { return !(a == b); }

std::ostream& operator<<(std::ostream& os, const Value& v) { return os << v.to_sql(); }

std::optional<int> compare(const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return std::nullopt;

    if (a.is_number() && b.is_number()) {
        const double x = a.as_double();
        const double y = b.as_double();
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }

    if (!a.is_number() && !b.is_number()) {
        const std::string& x = a.as_text();
        const std::string& y = b.as_text();
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }

    // Mixed types: numbers sort before text (SQLite's storage-class ordering).
    return a.is_number() ? -1 : 1;
}

}  // namespace minidb
