#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace minidb {

// Base class for every error MiniDB reports to the user.
class DbError : public std::runtime_error {
public:
    explicit DbError(const std::string& message) : std::runtime_error(message) {}
};

// An error that can be pinned to a location in the SQL source text.
class SqlError : public DbError {
public:
    SqlError(const std::string& message, std::size_t pos, std::size_t line, std::size_t col)
        : DbError(message), pos_(pos), line_(line), col_(col) {}

    std::size_t pos() const { return pos_; }
    std::size_t line() const { return line_; }
    std::size_t col() const { return col_; }

private:
    std::size_t pos_;
    std::size_t line_;
    std::size_t col_;
};

// Renders a diagnostic with the offending line and a caret, e.g.
//
//   error: expected FROM, found 'WHERE'
//     --> line 1, column 12
//      | SELECT name WHERE id = 1
//      |             ^
std::string render_error(const std::string& source, const SqlError& err);

}  // namespace minidb
