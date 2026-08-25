#include "minidb/ast.hpp"

namespace minidb {

const char* binary_op_symbol(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Concat: return "||";
        case BinaryOp::Eq: return "=";
        case BinaryOp::NotEq: return "<>";
        case BinaryOp::Lt: return "<";
        case BinaryOp::LtEq: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::GtEq: return ">=";
        case BinaryOp::And: return "AND";
        case BinaryOp::Or: return "OR";
        case BinaryOp::Is: return "IS";
        case BinaryOp::IsNot: return "IS NOT";
    }
    return "?";
}

ExprPtr Expr::make_literal(Value v, std::size_t pos) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Literal;
    e->literal = std::move(v);
    e->pos = pos;
    return e;
}

ExprPtr Expr::make_column(std::string name, std::size_t pos) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Column;
    e->column = std::move(name);
    e->pos = pos;
    return e;
}

ExprPtr Expr::make_unary(UnaryOp op, ExprPtr operand, std::size_t pos) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Unary;
    e->unary_op = op;
    e->left = std::move(operand);
    e->pos = pos;
    return e;
}

ExprPtr Expr::make_binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs, std::size_t pos) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Binary;
    e->binary_op = op;
    e->left = std::move(lhs);
    e->right = std::move(rhs);
    e->pos = pos;
    return e;
}

std::string to_string(const Expr& e) {
    switch (e.kind) {
        case ExprKind::Literal:
            return e.literal.to_sql();
        case ExprKind::Column:
            return e.column;
        case ExprKind::Unary: {
            const std::string prefix = (e.unary_op == UnaryOp::Negate) ? "-" : "NOT ";
            return "(" + prefix + to_string(*e.left) + ")";
        }
        case ExprKind::Binary:
            return "(" + to_string(*e.left) + " " + binary_op_symbol(e.binary_op) + " " +
                   to_string(*e.right) + ")";
    }
    return "<?>";
}

}  // namespace minidb
