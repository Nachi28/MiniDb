#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "minidb/token.hpp"

namespace minidb {

// Hand-written lexer. Single pass, no regex, tracks line/column for diagnostics.
class Lexer {
public:
    explicit Lexer(std::string source);

    Token next();                   // produces EndOfInput forever once exhausted
    std::vector<Token> tokenize();  // all tokens, terminated by EndOfInput

    const std::string& source() const { return source_; }

private:
    bool at_end() const;
    char peek(std::size_t offset = 0) const;
    char advance();
    void skip_trivia();

    Token lex_identifier_or_keyword();
    Token lex_number();
    Token lex_string();
    Token lex_quoted_identifier();

    [[noreturn]] void fail(const std::string& message, std::size_t pos, std::size_t line,
                           std::size_t col) const;

    std::string source_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t col_ = 1;
};

}  // namespace minidb
