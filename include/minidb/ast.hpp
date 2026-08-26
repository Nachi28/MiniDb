#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "minidb/value.hpp"

namespace minidb {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class ExprKind { Literal, Column, Unary, Binary, FunctionCall };
enum class UnaryOp { Negate, Not };
enum class BinaryOp {
    Add, Sub, Mul, Div, Mod, Concat,
    Eq, NotEq, Lt, LtEq, Gt, GtEq,
    And, Or, Is, IsNot
};

const char* binary_op_symbol(BinaryOp op);

// One node type with a discriminant, rather than a class hierarchy: easier to
// extend during phases 2-5 and it keeps the whole AST in one cache-friendly shape.
struct Expr {
    ExprKind kind = ExprKind::Literal;
    Value literal;             // Literal
    std::string column;        // Column
    UnaryOp unary_op = UnaryOp::Negate;
    BinaryOp binary_op = BinaryOp::Add;
    ExprPtr left;              // Unary operand / Binary lhs
    ExprPtr right;             // Binary rhs
    std::string function;      // FunctionCall: name as written
    std::vector<ExprPtr> args; // FunctionCall arguments
    bool star_arg = false;     // FunctionCall: the '*' in COUNT(*)
    std::size_t pos = 0;

    static ExprPtr make_literal(Value v, std::size_t pos);
    static ExprPtr make_column(std::string name, std::size_t pos);
    static ExprPtr make_unary(UnaryOp op, ExprPtr operand, std::size_t pos);
    static ExprPtr make_binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs, std::size_t pos);
    static ExprPtr make_call(std::string name, std::vector<ExprPtr> args, bool star_arg,
                             std::size_t pos);
};

// Fully parenthesised rendering. Used by tests to assert precedence and by
// SELECT to name unaliased result columns.
std::string to_string(const Expr& e);

struct ColumnDef {
    std::string name;
    ValueType type = ValueType::Text;
};

struct CreateTableStmt {
    std::string table;
    std::vector<ColumnDef> columns;
    bool if_not_exists = false;
};

struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;          // empty => every column, declared order
    std::vector<std::vector<ExprPtr>> rows;
};

struct SelectItem {
    bool star = false;
    ExprPtr expr;        // null when star
    std::string alias;   // empty when not aliased
};

struct OrderTerm {
    ExprPtr expr;
    bool descending = false;
};

struct SelectStmt {
    std::vector<SelectItem> items;
    std::string table;             // empty => no FROM clause
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    ExprPtr having;
    std::vector<OrderTerm> order_by;
    std::int64_t limit = -1;       // -1 => unlimited
};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt>;

}  // namespace minidb
