#include "minidb/parser.hpp"

#include <optional>
#include <utility>

#include "minidb/diagnostic.hpp"

namespace minidb {
namespace {

std::string describe_token(const Token& tok) {
    if (tok.type == TokenType::EndOfInput) return "end of input";
    return "'" + tok.lexeme + "'";
}

// Binding powers for the Pratt loop. Left < right makes an operator
// left-associative; the gaps leave room for new operators in later phases.
//
//   OR            1/2
//   AND           3/4
//   NOT (prefix)  5
//   comparisons   7/8      (=  <>  <  <=  >  >=  IS  IS NOT)
//   ||            9/10
//   + -           11/12
//   * / %         13/14
//   - (prefix)    15
struct BinInfo {
    BinaryOp op;
    int lbp;
    int rbp;
};

constexpr int kComparisonLbp = 7;
constexpr int kComparisonRbp = 8;
constexpr int kNotBindingPower = 5;
constexpr int kNegateBindingPower = 15;

std::optional<BinInfo> binary_info(TokenType type) {
    switch (type) {
        case TokenType::KwOr: return BinInfo{BinaryOp::Or, 1, 2};
        case TokenType::KwAnd: return BinInfo{BinaryOp::And, 3, 4};
        case TokenType::Eq: return BinInfo{BinaryOp::Eq, kComparisonLbp, kComparisonRbp};
        case TokenType::NotEq: return BinInfo{BinaryOp::NotEq, kComparisonLbp, kComparisonRbp};
        case TokenType::Lt: return BinInfo{BinaryOp::Lt, kComparisonLbp, kComparisonRbp};
        case TokenType::LtEq: return BinInfo{BinaryOp::LtEq, kComparisonLbp, kComparisonRbp};
        case TokenType::Gt: return BinInfo{BinaryOp::Gt, kComparisonLbp, kComparisonRbp};
        case TokenType::GtEq: return BinInfo{BinaryOp::GtEq, kComparisonLbp, kComparisonRbp};
        case TokenType::Concat: return BinInfo{BinaryOp::Concat, 9, 10};
        case TokenType::Plus: return BinInfo{BinaryOp::Add, 11, 12};
        case TokenType::Minus: return BinInfo{BinaryOp::Sub, 11, 12};
        case TokenType::Star: return BinInfo{BinaryOp::Mul, 13, 14};
        case TokenType::Slash: return BinInfo{BinaryOp::Div, 13, 14};
        case TokenType::Percent: return BinInfo{BinaryOp::Mod, 13, 14};
        default: return std::nullopt;
    }
}

}  // namespace

Parser::Parser(std::string source) : source_(std::move(source)) {
    Lexer lexer(source_);
    tokens_ = lexer.tokenize();
}

const Token& Parser::peek(std::size_t offset) const {
    const std::size_t i = index_ + offset;
    return i < tokens_.size() ? tokens_[i] : tokens_.back();
}

const Token& Parser::current() const { return peek(0); }

Token Parser::advance() {
    Token tok = current();
    if (index_ + 1 < tokens_.size()) ++index_;
    return tok;
}

bool Parser::check(TokenType t) const { return current().type == t; }

bool Parser::match(TokenType t) {
    if (!check(t)) return false;
    advance();
    return true;
}

Token Parser::expect(TokenType t, const std::string& what) {
    if (!check(t)) {
        fail_at(current(), "expected " + what + ", found " + describe_token(current()));
    }
    return advance();
}

void Parser::fail(const std::string& message) const { fail_at(current(), message); }

void Parser::fail_at(const Token& tok, const std::string& message) const {
    throw SqlError(message, tok.pos, tok.line, tok.col);
}

std::string Parser::parse_identifier(const std::string& what) {
    if (!check(TokenType::Identifier)) {
        fail_at(current(), "expected " + what + ", found " + describe_token(current()));
    }
    return advance().lexeme;
}

std::vector<Statement> Parser::parse_program() {
    std::vector<Statement> statements;
    while (!check(TokenType::EndOfInput)) {
        if (match(TokenType::Semicolon)) continue;  // tolerate stray/empty statements
        statements.push_back(parse_statement());
        if (!check(TokenType::EndOfInput)) {
            expect(TokenType::Semicolon, "';' after statement");
        }
    }
    return statements;
}

Statement Parser::parse_statement() {
    if (check(TokenType::KwCreate)) return parse_create();
    if (check(TokenType::KwInsert)) return Statement(parse_insert());
    if (check(TokenType::KwSelect)) return Statement(parse_select());

    if (check(TokenType::KwExplain)) {
        advance();
        const bool analyze = match(TokenType::KwAnalyze);
        SelectStmt select = parse_select();
        select.explain = true;
        select.analyze = analyze;
        return Statement(std::move(select));
    }

    if (check(TokenType::KwBegin) || check(TokenType::KwCommit) ||
        check(TokenType::KwRollback)) {
        TransactionStmt stmt;
        const TokenType kind = advance().type;
        stmt.kind = kind == TokenType::KwBegin      ? TransactionStmt::Kind::Begin
                    : kind == TokenType::KwCommit   ? TransactionStmt::Kind::Commit
                                                    : TransactionStmt::Kind::Rollback;
        match(TokenType::KwTransaction);  // the noise word is optional
        return Statement(stmt);
    }

    fail("expected CREATE, INSERT, SELECT, EXPLAIN, BEGIN, COMMIT or ROLLBACK, found " +
         describe_token(current()));
}

ExprPtr Parser::parse_expression() { return parse_expr_bp(0); }

// CREATE splits two ways, so the keyword after it decides the statement.
Statement Parser::parse_create() {
    if (peek(1).type == TokenType::KwIndex) return Statement(parse_create_index());
    return Statement(parse_create_table());
}

CreateIndexStmt Parser::parse_create_index() {
    expect(TokenType::KwCreate, "CREATE");
    expect(TokenType::KwIndex, "INDEX after CREATE");

    CreateIndexStmt stmt;
    if (match(TokenType::KwIf)) {
        expect(TokenType::KwNot, "NOT in IF NOT EXISTS");
        expect(TokenType::KwExists, "EXISTS in IF NOT EXISTS");
        stmt.if_not_exists = true;
    }
    stmt.name = parse_identifier("an index name");
    expect(TokenType::KwOn, "ON after the index name");
    stmt.table = parse_identifier("a table name");
    expect(TokenType::LParen, "'(' before the indexed column");
    stmt.column = parse_identifier("a column name");
    expect(TokenType::RParen, "')' after the indexed column");
    return stmt;
}

CreateTableStmt Parser::parse_create_table() {
    expect(TokenType::KwCreate, "CREATE");
    expect(TokenType::KwTable, "TABLE after CREATE");

    CreateTableStmt stmt;
    if (match(TokenType::KwIf)) {
        expect(TokenType::KwNot, "NOT in IF NOT EXISTS");
        expect(TokenType::KwExists, "EXISTS in IF NOT EXISTS");
        stmt.if_not_exists = true;
    }

    stmt.table = parse_identifier("a table name");
    expect(TokenType::LParen, "'(' before the column list");

    do {
        ColumnDef column;
        column.name = parse_identifier("a column name");
        switch (current().type) {
            case TokenType::KwInteger:
                column.type = ValueType::Integer;
                break;
            case TokenType::KwReal:
                column.type = ValueType::Real;
                break;
            case TokenType::KwText:
                column.type = ValueType::Text;
                break;
            default:
                fail_at(current(), "expected a column type (INTEGER, REAL or TEXT), found " +
                                       describe_token(current()));
        }
        advance();
        stmt.columns.push_back(std::move(column));
    } while (match(TokenType::Comma));

    expect(TokenType::RParen, "')' after the column list");
    return stmt;
}

InsertStmt Parser::parse_insert() {
    expect(TokenType::KwInsert, "INSERT");
    expect(TokenType::KwInto, "INTO after INSERT");

    InsertStmt stmt;
    stmt.table = parse_identifier("a table name");

    if (match(TokenType::LParen)) {
        do {
            stmt.columns.push_back(parse_identifier("a column name"));
        } while (match(TokenType::Comma));
        expect(TokenType::RParen, "')' after the column list");
    }

    expect(TokenType::KwValues, "VALUES");
    do {
        expect(TokenType::LParen, "'(' before a row of values");
        std::vector<ExprPtr> row;
        if (!check(TokenType::RParen)) {
            do {
                row.push_back(parse_expr_bp(0));
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RParen, "')' after a row of values");
        stmt.rows.push_back(std::move(row));
    } while (match(TokenType::Comma));

    return stmt;
}

SelectStmt Parser::parse_select() {
    expect(TokenType::KwSelect, "SELECT");

    SelectStmt stmt;
    do {
        SelectItem item;
        if (match(TokenType::Star)) {
            item.star = true;
        } else {
            item.expr = parse_expr_bp(0);
            if (match(TokenType::KwAs)) {
                item.alias = parse_identifier("an alias after AS");
            } else if (check(TokenType::Identifier)) {
                item.alias = advance().lexeme;  // implicit alias: SELECT a b
            }
        }
        stmt.items.push_back(std::move(item));
    } while (match(TokenType::Comma));

    if (match(TokenType::KwFrom)) {
        stmt.table = parse_identifier("a table name after FROM");
    }

    if (match(TokenType::KwWhere)) {
        stmt.where = parse_expr_bp(0);
    }

    if (match(TokenType::KwGroup)) {
        expect(TokenType::KwBy, "BY after GROUP");
        do {
            stmt.group_by.push_back(parse_expr_bp(0));
        } while (match(TokenType::Comma));
    }

    if (match(TokenType::KwHaving)) {
        stmt.having = parse_expr_bp(0);
    }

    if (match(TokenType::KwOrder)) {
        expect(TokenType::KwBy, "BY after ORDER");
        do {
            OrderTerm term;
            term.expr = parse_expr_bp(0);
            if (match(TokenType::KwDesc)) {
                term.descending = true;
            } else {
                match(TokenType::KwAsc);  // optional, and the default
            }
            stmt.order_by.push_back(std::move(term));
        } while (match(TokenType::Comma));
    }

    if (match(TokenType::KwLimit)) {
        if (!check(TokenType::IntegerLit)) {
            fail_at(current(), "expected an integer after LIMIT, found " +
                                   describe_token(current()));
        }
        stmt.limit = advance().literal.as_integer();
    }

    return stmt;
}

// Pratt loop: parse a prefix, then keep folding in infix operators whose left
// binding power is at least min_bp.
ExprPtr Parser::parse_expr_bp(int min_bp) {
    ExprPtr lhs = parse_prefix();

    for (;;) {
        // IS / IS NOT are two tokens, so they are handled ahead of the table.
        if (check(TokenType::KwIs)) {
            if (kComparisonLbp < min_bp) break;
            const Token op_tok = advance();
            const bool negated = match(TokenType::KwNot);
            ExprPtr rhs = parse_expr_bp(kComparisonRbp);
            lhs = Expr::make_binary(negated ? BinaryOp::IsNot : BinaryOp::Is, std::move(lhs),
                                    std::move(rhs), op_tok.pos);
            continue;
        }

        const std::optional<BinInfo> info = binary_info(current().type);
        if (!info.has_value() || info->lbp < min_bp) break;

        const Token op_tok = advance();
        ExprPtr rhs = parse_expr_bp(info->rbp);
        lhs = Expr::make_binary(info->op, std::move(lhs), std::move(rhs), op_tok.pos);
    }

    return lhs;
}

ExprPtr Parser::parse_prefix() {
    switch (current().type) {
        case TokenType::IntegerLit:
        case TokenType::RealLit:
        case TokenType::StringLit: {
            const Token tok = advance();
            return Expr::make_literal(tok.literal, tok.pos);
        }
        case TokenType::KwNull: {
            const Token tok = advance();
            return Expr::make_literal(Value::null(), tok.pos);
        }
        case TokenType::Identifier: {
            const Token tok = advance();
            if (!check(TokenType::LParen)) return Expr::make_column(tok.lexeme, tok.pos);

            advance();  // '('
            std::vector<ExprPtr> args;
            bool star_arg = false;
            if (match(TokenType::Star)) {
                star_arg = true;  // COUNT(*)
            } else if (!check(TokenType::RParen)) {
                do {
                    args.push_back(parse_expr_bp(0));
                } while (match(TokenType::Comma));
            }
            expect(TokenType::RParen, "')' to close the argument list");
            return Expr::make_call(tok.lexeme, std::move(args), star_arg, tok.pos);
        }
        case TokenType::Minus: {
            const Token tok = advance();
            return Expr::make_unary(UnaryOp::Negate, parse_expr_bp(kNegateBindingPower), tok.pos);
        }
        case TokenType::Plus: {
            advance();  // unary plus is a no-op
            return parse_expr_bp(kNegateBindingPower);
        }
        case TokenType::KwNot: {
            const Token tok = advance();
            return Expr::make_unary(UnaryOp::Not, parse_expr_bp(kNotBindingPower), tok.pos);
        }
        case TokenType::LParen: {
            advance();
            ExprPtr inner = parse_expr_bp(0);
            expect(TokenType::RParen, "')' to close the expression");
            return inner;
        }
        default:
            fail_at(current(), "expected an expression, found " + describe_token(current()));
    }
}

}  // namespace minidb
