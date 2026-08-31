#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "minidb/ast.hpp"
#include "minidb/lexer.hpp"

namespace minidb {

// Recursive descent for statements, Pratt (precedence climbing) for expressions.
class Parser {
public:
    explicit Parser(std::string source);

    std::vector<Statement> parse_program();  // ';'-separated statements
    Statement parse_statement();
    ExprPtr parse_expression();              // exposed so tests can drive it directly

    const std::string& source() const { return source_; }

private:
    const Token& peek(std::size_t offset = 0) const;
    const Token& current() const;
    Token advance();
    bool check(TokenType t) const;
    bool match(TokenType t);
    Token expect(TokenType t, const std::string& what);

    [[noreturn]] void fail(const std::string& message) const;
    [[noreturn]] void fail_at(const Token& tok, const std::string& message) const;

    Statement parse_create();
    CreateTableStmt parse_create_table();
    CreateIndexStmt parse_create_index();
    InsertStmt parse_insert();
    SelectStmt parse_select();

    ExprPtr parse_expr_bp(int min_bp);
    ExprPtr parse_prefix();
    std::string parse_identifier(const std::string& what);

    std::string source_;
    std::vector<Token> tokens_;
    std::size_t index_ = 0;
};

}  // namespace minidb
