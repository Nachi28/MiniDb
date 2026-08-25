#include "minidb/diagnostic.hpp"

#include <sstream>

namespace minidb {

std::string render_error(const std::string& source, const SqlError& err) {
    const std::size_t pos = err.pos() > source.size() ? source.size() : err.pos();

    std::size_t line_start = 0;
    for (std::size_t i = 0; i < pos; ++i) {
        if (source[i] == '\n') line_start = i + 1;
    }
    std::size_t line_end = source.find('\n', line_start);
    if (line_end == std::string::npos) line_end = source.size();

    const std::string line_text = source.substr(line_start, line_end - line_start);
    const std::size_t caret_col = pos - line_start;

    std::ostringstream oss;
    oss << "error: " << err.what() << "\n";
    oss << "  --> line " << err.line() << ", column " << err.col() << "\n";
    oss << "   | " << line_text << "\n";
    oss << "   | " << std::string(caret_col, ' ') << "^";
    return oss.str();
}

}  // namespace minidb
