#include "minidb/lexer.hpp"

#include <cctype>
#include <cstdlib>
#include <map>

#include "minidb/diagnostic.hpp"
#include "minidb/util.hpp"

namespace minidb {
namespace {

const std::map<std::string, TokenType>& keyword_table() {
    static const std::map<std::string, TokenType> table = {
        {"select", TokenType::KwSelect},   {"from", TokenType::KwFrom},
        {"where", TokenType::KwWhere},     {"insert", TokenType::KwInsert},
        {"into", TokenType::KwInto},       {"values", TokenType::KwValues},
        {"create", TokenType::KwCreate},   {"table", TokenType::KwTable},
        {"and", TokenType::KwAnd},         {"or", TokenType::KwOr},
        {"not", TokenType::KwNot},         {"null", TokenType::KwNull},
        {"is", TokenType::KwIs},           {"integer", TokenType::KwInteger},
        {"int", TokenType::KwInteger},     {"real", TokenType::KwReal},
        {"float", TokenType::KwReal},      {"double", TokenType::KwReal},
        {"text", TokenType::KwText},       {"varchar", TokenType::KwText},
        {"string", TokenType::KwText},     {"order", TokenType::KwOrder},
        {"by", TokenType::KwBy},           {"asc", TokenType::KwAsc},
        {"desc", TokenType::KwDesc},       {"limit", TokenType::KwLimit},
        {"as", TokenType::KwAs},           {"if", TokenType::KwIf},
        {"exists", TokenType::KwExists},
    };
    return table;
}

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

}  // namespace

const char* token_type_name(TokenType t) {
    switch (t) {
        case TokenType::Identifier: return "identifier";
        case TokenType::IntegerLit: return "integer literal";
        case TokenType::RealLit: return "real literal";
        case TokenType::StringLit: return "string literal";
        case TokenType::KwSelect: return "SELECT";
        case TokenType::KwFrom: return "FROM";
        case TokenType::KwWhere: return "WHERE";
        case TokenType::KwInsert: return "INSERT";
        case TokenType::KwInto: return "INTO";
        case TokenType::KwValues: return "VALUES";
        case TokenType::KwCreate: return "CREATE";
        case TokenType::KwTable: return "TABLE";
        case TokenType::KwAnd: return "AND";
        case TokenType::KwOr: return "OR";
        case TokenType::KwNot: return "NOT";
        case TokenType::KwNull: return "NULL";
        case TokenType::KwIs: return "IS";
        case TokenType::KwInteger: return "INTEGER";
        case TokenType::KwReal: return "REAL";
        case TokenType::KwText: return "TEXT";
        case TokenType::KwOrder: return "ORDER";
        case TokenType::KwBy: return "BY";
        case TokenType::KwAsc: return "ASC";
        case TokenType::KwDesc: return "DESC";
        case TokenType::KwLimit: return "LIMIT";
        case TokenType::KwAs: return "AS";
        case TokenType::KwIf: return "IF";
        case TokenType::KwExists: return "EXISTS";
        case TokenType::Star: return "star";
        case TokenType::Comma: return "comma";
        case TokenType::LParen: return "open paren";
        case TokenType::RParen: return "close paren";
        case TokenType::Semicolon: return "semicolon";
        case TokenType::Eq: return "equals";
        case TokenType::NotEq: return "not-equals";
        case TokenType::Lt: return "less-than";
        case TokenType::LtEq: return "less-or-equal";
        case TokenType::Gt: return "greater-than";
        case TokenType::GtEq: return "greater-or-equal";
        case TokenType::Plus: return "plus";
        case TokenType::Minus: return "minus";
        case TokenType::Slash: return "slash";
        case TokenType::Percent: return "percent";
        case TokenType::Concat: return "concat";
        case TokenType::EndOfInput: return "end of input";
    }
    return "unknown token";
}

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

bool Lexer::at_end() const { return pos_ >= source_.size(); }

char Lexer::peek(std::size_t offset) const {
    const std::size_t i = pos_ + offset;
    return i < source_.size() ? source_[i] : '\0';
}

char Lexer::advance() {
    const char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

void Lexer::fail(const std::string& message, std::size_t pos, std::size_t line,
                 std::size_t col) const {
    throw SqlError(message, pos, line, col);
}

void Lexer::skip_trivia() {
    for (;;) {
        if (at_end()) return;
        const char c = peek();

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }

        if (c == '-' && peek(1) == '-') {  // line comment, runs to end of line
            while (!at_end() && peek() != '\n') advance();
            continue;
        }

        if (c == '/' && peek(1) == '*') {  // block comment
            const std::size_t start = pos_;
            const std::size_t start_line = line_;
            const std::size_t start_col = col_;
            advance();
            advance();
            for (;;) {
                if (at_end()) fail("unterminated block comment", start, start_line, start_col);
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    break;
                }
                advance();
            }
            continue;
        }

        return;
    }
}

Token Lexer::lex_identifier_or_keyword() {
    const std::size_t start = pos_;
    const std::size_t start_line = line_;
    const std::size_t start_col = col_;

    while (!at_end() && is_ident_char(peek())) advance();

    Token tok;
    tok.lexeme = source_.substr(start, pos_ - start);
    tok.pos = start;
    tok.line = start_line;
    tok.col = start_col;

    const auto& table = keyword_table();
    const auto it = table.find(to_lower(tok.lexeme));
    tok.type = (it == table.end()) ? TokenType::Identifier : it->second;
    return tok;
}

Token Lexer::lex_number() {
    const std::size_t start = pos_;
    const std::size_t start_line = line_;
    const std::size_t start_col = col_;
    bool is_real = false;

    while (!at_end() && is_digit(peek())) advance();

    if (!at_end() && peek() == '.' && is_digit(peek(1))) {
        is_real = true;
        advance();
        while (!at_end() && is_digit(peek())) advance();
    }

    if (!at_end() && (peek() == 'e' || peek() == 'E')) {
        const std::size_t save_pos = pos_;
        const std::size_t save_line = line_;
        const std::size_t save_col = col_;
        advance();
        if (!at_end() && (peek() == '+' || peek() == '-')) advance();
        if (!at_end() && is_digit(peek())) {
            is_real = true;
            while (!at_end() && is_digit(peek())) advance();
        } else {
            pos_ = save_pos;  // not an exponent after all, e.g. `1 END`
            line_ = save_line;
            col_ = save_col;
        }
    }

    if (!at_end() && is_ident_start(peek())) {
        fail("invalid numeric literal", start, start_line, start_col);
    }

    Token tok;
    tok.lexeme = source_.substr(start, pos_ - start);
    tok.pos = start;
    tok.line = start_line;
    tok.col = start_col;

    if (is_real) {
        tok.type = TokenType::RealLit;
        tok.literal = Value::real(std::strtod(tok.lexeme.c_str(), nullptr));
    } else {
        tok.type = TokenType::IntegerLit;
        tok.literal = Value::integer(
            static_cast<std::int64_t>(std::strtoll(tok.lexeme.c_str(), nullptr, 10)));
    }
    return tok;
}

Token Lexer::lex_string() {
    const std::size_t start = pos_;
    const std::size_t start_line = line_;
    const std::size_t start_col = col_;

    advance();  // opening quote
    std::string contents;
    for (;;) {
        if (at_end()) fail("unterminated string literal", start, start_line, start_col);
        const char c = advance();
        if (c == '\'') {
            if (!at_end() && peek() == '\'') {  // '' is an escaped quote
                advance();
                contents += '\'';
                continue;
            }
            break;
        }
        contents += c;
    }

    Token tok;
    tok.type = TokenType::StringLit;
    tok.lexeme = source_.substr(start, pos_ - start);
    tok.literal = Value::text(contents);
    tok.pos = start;
    tok.line = start_line;
    tok.col = start_col;
    return tok;
}

Token Lexer::lex_quoted_identifier() {
    const std::size_t start = pos_;
    const std::size_t start_line = line_;
    const std::size_t start_col = col_;

    advance();  // opening double quote
    std::string contents;
    for (;;) {
        if (at_end()) fail("unterminated quoted identifier", start, start_line, start_col);
        const char c = advance();
        if (c == '"') {
            if (!at_end() && peek() == '"') {
                advance();
                contents += '"';
                continue;
            }
            break;
        }
        contents += c;
    }

    Token tok;
    tok.type = TokenType::Identifier;
    tok.lexeme = contents;  // unquoted name, so the parser can use it directly
    tok.pos = start;
    tok.line = start_line;
    tok.col = start_col;
    return tok;
}

Token Lexer::next() {
    skip_trivia();

    if (at_end()) {
        Token tok;
        tok.type = TokenType::EndOfInput;
        tok.pos = pos_;
        tok.line = line_;
        tok.col = col_;
        return tok;
    }

    const char c = peek();
    if (is_ident_start(c)) return lex_identifier_or_keyword();
    if (is_digit(c)) return lex_number();
    if (c == '\'') return lex_string();
    if (c == '"') return lex_quoted_identifier();

    const std::size_t start = pos_;
    const std::size_t start_line = line_;
    const std::size_t start_col = col_;
    advance();

    TokenType type = TokenType::EndOfInput;
    switch (c) {
        case '*': type = TokenType::Star; break;
        case ',': type = TokenType::Comma; break;
        case '(': type = TokenType::LParen; break;
        case ')': type = TokenType::RParen; break;
        case ';': type = TokenType::Semicolon; break;
        case '+': type = TokenType::Plus; break;
        case '-': type = TokenType::Minus; break;
        case '/': type = TokenType::Slash; break;
        case '%': type = TokenType::Percent; break;
        case '=':
            if (peek() == '=') advance();  // accept == as an alias for =
            type = TokenType::Eq;
            break;
        case '<':
            if (peek() == '=') {
                advance();
                type = TokenType::LtEq;
            } else if (peek() == '>') {
                advance();
                type = TokenType::NotEq;
            } else {
                type = TokenType::Lt;
            }
            break;
        case '>':
            if (peek() == '=') {
                advance();
                type = TokenType::GtEq;
            } else {
                type = TokenType::Gt;
            }
            break;
        case '!':
            if (peek() != '=') {
                fail("unexpected character, did you mean !=", start, start_line, start_col);
            }
            advance();
            type = TokenType::NotEq;
            break;
        case '|':
            if (peek() != '|') {
                fail("unexpected character, did you mean ||", start, start_line, start_col);
            }
            advance();
            type = TokenType::Concat;
            break;
        default: {
            std::string message = "unexpected character ";
            message += c;
            fail(message, start, start_line, start_col);
        }
    }

    Token tok;
    tok.type = type;
    tok.lexeme = source_.substr(start, pos_ - start);
    tok.pos = start;
    tok.line = start_line;
    tok.col = start_col;
    return tok;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    for (;;) {
        Token tok = next();
        const bool done = tok.type == TokenType::EndOfInput;
        tokens.push_back(std::move(tok));
        if (done) break;
    }
    return tokens;
}

}  // namespace minidb
