#pragma once

#include <cstddef>
#include <string>

#include "minidb/value.hpp"

namespace minidb {

enum class TokenType {
    // Literals and identifiers
    Identifier,
    IntegerLit,
    RealLit,
    StringLit,

    // Keywords
    KwSelect, KwFrom, KwWhere, KwInsert, KwInto, KwValues,
    KwCreate, KwTable, KwAnd, KwOr, KwNot, KwNull, KwIs,
    KwInteger, KwReal, KwText, KwOrder, KwBy, KwAsc, KwDesc,
    KwLimit, KwAs, KwIf, KwExists, KwGroup, KwHaving,

    // Punctuation and operators
    Star, Comma, LParen, RParen, Semicolon,
    Eq, NotEq, Lt, LtEq, Gt, GtEq,
    Plus, Minus, Slash, Percent, Concat,

    EndOfInput
};

const char* token_type_name(TokenType t);

struct Token {
    TokenType type = TokenType::EndOfInput;
    std::string lexeme;   // source text (for quoted identifiers, the unquoted name)
    Value literal;        // populated for IntegerLit / RealLit / StringLit
    std::size_t pos = 0;  // byte offset of the first character
    std::size_t line = 1;
    std::size_t col = 1;
};

}  // namespace minidb
