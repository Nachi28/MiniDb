#include "minidb/executor.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "minidb/diagnostic.hpp"
#include "minidb/util.hpp"

namespace minidb {
namespace {

Value eval_binary(const Expr& expr, const Table* table, const Row* row);

// Resolves every column reference in an expression against the table schema.
// This runs before the scan, so an unknown column is reported even when the
// table is empty and no row would ever be evaluated.
void validate_columns(const Expr& expr, const Table* table) {
    switch (expr.kind) {
        case ExprKind::Literal:
            return;
        case ExprKind::Column:
            if (table == nullptr) throw DbError("no such column: " + expr.column);
            if (table->column_index(expr.column) < 0) {
                throw DbError("no such column: " + expr.column + " in table " +
                              table->name());
            }
            return;
        case ExprKind::Unary:
            validate_columns(*expr.left, table);
            return;
        case ExprKind::Binary:
            validate_columns(*expr.left, table);
            validate_columns(*expr.right, table);
            return;
    }
}

// Narrowing a value to the declared type of the column it is being stored in.
// Phase 1 is strict about text/number confusion; SQLite would coerce instead.
Value coerce(const Value& v, ValueType target, const std::string& column) {
    if (v.is_null()) return v;
    switch (target) {
        case ValueType::Integer:
            if (v.type() == ValueType::Integer) return v;
            if (v.type() == ValueType::Real) return Value::integer(v.as_integer());
            break;
        case ValueType::Real:
            if (v.is_number()) return Value::real(v.as_double());
            break;
        case ValueType::Text:
            if (v.type() == ValueType::Text) return v;
            break;
        case ValueType::Null:
            return v;
    }
    throw DbError(std::string("cannot store ") + type_name(v.type()) + " in " +
                  type_name(target) + " column '" + column + "'");
}

}  // namespace

bool is_true(const Value& v) {
    if (v.is_null()) return false;      // NULL is unknown, and unknown is not true
    if (v.is_number()) return v.as_double() != 0.0;
    return false;                       // TEXT is never a condition on its own
}

Value evaluate(const Expr& expr, const Table* table, const Row* row) {
    switch (expr.kind) {
        case ExprKind::Literal:
            return expr.literal;

        case ExprKind::Column: {
            if (table == nullptr || row == nullptr) {
                throw DbError("no such column: " + expr.column);
            }
            const int index = table->column_index(expr.column);
            if (index < 0) {
                throw DbError("no such column: " + expr.column + " in table " + table->name());
            }
            const std::size_t i = static_cast<std::size_t>(index);
            return i < row->size() ? (*row)[i] : Value::null();
        }

        case ExprKind::Unary: {
            const Value v = evaluate(*expr.left, table, row);
            if (expr.unary_op == UnaryOp::Negate) {
                if (v.is_null()) return Value::null();
                if (!v.is_number()) throw DbError("cannot negate a TEXT value");
                if (v.type() == ValueType::Integer) return Value::integer(-v.as_integer());
                return Value::real(-v.as_double());
            }
            if (v.is_null()) return Value::null();  // NOT NULL is NULL, not TRUE
            return Value::boolean(!is_true(v));
        }

        case ExprKind::Binary:
            return eval_binary(expr, table, row);
    }
    throw DbError("internal error: unhandled expression kind");
}

namespace {

Value eval_binary(const Expr& expr, const Table* table, const Row* row) {
    const BinaryOp op = expr.binary_op;

    // AND / OR use SQL three-valued logic, so they cannot be folded into the
    // generic path below: FALSE AND NULL is FALSE, but TRUE AND NULL is NULL.
    if (op == BinaryOp::And || op == BinaryOp::Or) {
        const Value lhs = evaluate(*expr.left, table, row);
        if (op == BinaryOp::And) {
            if (!lhs.is_null() && !is_true(lhs)) return Value::boolean(false);
            const Value rhs = evaluate(*expr.right, table, row);
            if (!rhs.is_null() && !is_true(rhs)) return Value::boolean(false);
            if (lhs.is_null() || rhs.is_null()) return Value::null();
            return Value::boolean(true);
        }
        if (!lhs.is_null() && is_true(lhs)) return Value::boolean(true);
        const Value rhs = evaluate(*expr.right, table, row);
        if (!rhs.is_null() && is_true(rhs)) return Value::boolean(true);
        if (lhs.is_null() || rhs.is_null()) return Value::null();
        return Value::boolean(false);
    }

    const Value lhs = evaluate(*expr.left, table, row);
    const Value rhs = evaluate(*expr.right, table, row);

    // IS / IS NOT are the null-safe comparisons: NULL IS NULL is TRUE.
    if (op == BinaryOp::Is || op == BinaryOp::IsNot) {
        const bool same = (lhs == rhs);
        return Value::boolean(op == BinaryOp::Is ? same : !same);
    }

    switch (op) {
        case BinaryOp::Eq:
        case BinaryOp::NotEq:
        case BinaryOp::Lt:
        case BinaryOp::LtEq:
        case BinaryOp::Gt:
        case BinaryOp::GtEq: {
            const std::optional<int> c = compare(lhs, rhs);
            if (!c.has_value()) return Value::null();  // this is why x = NULL matches nothing
            const int r = *c;
            switch (op) {
                case BinaryOp::Eq: return Value::boolean(r == 0);
                case BinaryOp::NotEq: return Value::boolean(r != 0);
                case BinaryOp::Lt: return Value::boolean(r < 0);
                case BinaryOp::LtEq: return Value::boolean(r <= 0);
                case BinaryOp::Gt: return Value::boolean(r > 0);
                default: return Value::boolean(r >= 0);
            }
        }
        case BinaryOp::Concat:
            if (lhs.is_null() || rhs.is_null()) return Value::null();
            return Value::text(lhs.to_string() + rhs.to_string());
        default:
            break;
    }

    if (lhs.is_null() || rhs.is_null()) return Value::null();
    if (!lhs.is_number() || !rhs.is_number()) {
        throw DbError(std::string("cannot apply '") + binary_op_symbol(op) +
                      "' to a TEXT value");
    }

    const bool integral =
        lhs.type() == ValueType::Integer && rhs.type() == ValueType::Integer;

    if (op == BinaryOp::Div) {
        if (rhs.as_double() == 0.0) throw DbError("division by zero");
        if (integral) return Value::integer(lhs.as_integer() / rhs.as_integer());
        return Value::real(lhs.as_double() / rhs.as_double());
    }
    if (op == BinaryOp::Mod) {
        if (!integral) throw DbError("'%' requires INTEGER operands");
        if (rhs.as_integer() == 0) throw DbError("division by zero");
        return Value::integer(lhs.as_integer() % rhs.as_integer());
    }

    if (integral) {
        const std::int64_t a = lhs.as_integer();
        const std::int64_t b = rhs.as_integer();
        switch (op) {
            case BinaryOp::Add: return Value::integer(a + b);
            case BinaryOp::Sub: return Value::integer(a - b);
            case BinaryOp::Mul: return Value::integer(a * b);
            default: break;
        }
    } else {
        const double a = lhs.as_double();
        const double b = rhs.as_double();
        switch (op) {
            case BinaryOp::Add: return Value::real(a + b);
            case BinaryOp::Sub: return Value::real(a - b);
            case BinaryOp::Mul: return Value::real(a * b);
            default: break;
        }
    }

    throw DbError("internal error: unhandled binary operator");
}

}  // namespace

ResultSet Executor::execute(const Statement& stmt) {
    if (std::holds_alternative<CreateTableStmt>(stmt)) {
        return exec_create(std::get<CreateTableStmt>(stmt));
    }
    if (std::holds_alternative<InsertStmt>(stmt)) {
        return exec_insert(std::get<InsertStmt>(stmt));
    }
    return exec_select(std::get<SelectStmt>(stmt));
}

ResultSet Executor::exec_create(const CreateTableStmt& stmt) {
    ResultSet result;

    if (db_.find_table(stmt.table) != nullptr) {
        if (stmt.if_not_exists) {
            result.message = "table " + stmt.table + " already exists, nothing to do";
            return result;
        }
        throw DbError("table " + stmt.table + " already exists");
    }

    for (std::size_t i = 0; i < stmt.columns.size(); ++i) {
        for (std::size_t j = i + 1; j < stmt.columns.size(); ++j) {
            if (iequals(stmt.columns[i].name, stmt.columns[j].name)) {
                throw DbError("duplicate column name: " + stmt.columns[j].name);
            }
        }
    }

    db_.create_table(stmt.table, stmt.columns);
    result.message = "table " + stmt.table + " created";
    return result;
}

ResultSet Executor::exec_insert(const InsertStmt& stmt) {
    Table* table = db_.find_table(stmt.table);
    if (table == nullptr) throw DbError("no such table: " + stmt.table);

    // Map each supplied value position onto a column index in the table.
    std::vector<std::size_t> targets;
    if (stmt.columns.empty()) {
        for (std::size_t i = 0; i < table->columns().size(); ++i) targets.push_back(i);
    } else {
        for (const std::string& name : stmt.columns) {
            const int index = table->column_index(name);
            if (index < 0) {
                throw DbError("no such column: " + name + " in table " + table->name());
            }
            targets.push_back(static_cast<std::size_t>(index));
        }
    }

    std::size_t inserted = 0;
    for (const std::vector<ExprPtr>& row_exprs : stmt.rows) {
        if (row_exprs.size() != targets.size()) {
            throw DbError("table " + table->name() + " expects " +
                          std::to_string(targets.size()) + " value(s), but " +
                          std::to_string(row_exprs.size()) + " were supplied");
        }

        Row row(table->columns().size(), Value::null());
        for (std::size_t i = 0; i < row_exprs.size(); ++i) {
            const Value v = evaluate(*row_exprs[i], nullptr, nullptr);
            const ColumnDef& column = table->columns()[targets[i]];
            row[targets[i]] = coerce(v, column.type, column.name);
        }
        table->insert(std::move(row));
        ++inserted;
    }

    ResultSet result;
    result.message = std::to_string(inserted) + " row(s) inserted";
    return result;
}

ResultSet Executor::exec_select(const SelectStmt& stmt) {
    const Table* table = nullptr;
    if (!stmt.table.empty()) {
        table = db_.find_table(stmt.table);
        if (table == nullptr) throw DbError("no such table: " + stmt.table);
    }

    for (const SelectItem& item : stmt.items) {
        if (!item.star) validate_columns(*item.expr, table);
    }
    if (stmt.where) validate_columns(*stmt.where, table);
    for (const OrderTerm& term : stmt.order_by) validate_columns(*term.expr, table);

    static const Row kNoRow;  // stands in for the single row of a FROM-less SELECT

    // 1. Scan and filter. Rows are referenced, not copied, until projection.
    std::vector<const Row*> matched;
    if (table != nullptr) {
        for (const Row& row : table->rows()) {
            if (stmt.where && !is_true(evaluate(*stmt.where, table, &row))) continue;
            matched.push_back(&row);
        }
    } else if (!stmt.where || is_true(evaluate(*stmt.where, nullptr, nullptr))) {
        matched.push_back(&kNoRow);
    }

    // 2. ORDER BY. stable_sort keeps insertion order for equal keys.
    if (!stmt.order_by.empty()) {
        std::stable_sort(matched.begin(), matched.end(),
                         [&](const Row* a, const Row* b) {
                             for (const OrderTerm& term : stmt.order_by) {
                                 const Value va = evaluate(*term.expr, table, a);
                                 const Value vb = evaluate(*term.expr, table, b);
                                 int c = 0;
                                 if (va.is_null() && vb.is_null()) {
                                     c = 0;
                                 } else if (va.is_null()) {
                                     c = -1;  // NULLs sort first, as in SQLite
                                 } else if (vb.is_null()) {
                                     c = 1;
                                 } else {
                                     c = compare(va, vb).value_or(0);
                                 }
                                 if (c != 0) return term.descending ? c > 0 : c < 0;
                             }
                             return false;
                         });
    }

    // 3. LIMIT.
    if (stmt.limit >= 0) {
        const std::size_t limit = static_cast<std::size_t>(stmt.limit);
        if (limit < matched.size()) matched.resize(limit);
    }

    // 4. Projection.
    ResultSet result;
    result.is_query = true;

    for (const SelectItem& item : stmt.items) {
        if (item.star) {
            if (table == nullptr) throw DbError("SELECT * requires a FROM clause");
            for (const ColumnDef& column : table->columns()) {
                result.column_names.push_back(column.name);
            }
        } else if (!item.alias.empty()) {
            result.column_names.push_back(item.alias);
        } else {
            result.column_names.push_back(to_string(*item.expr));
        }
    }

    for (const Row* row : matched) {
        Row out;
        out.reserve(result.column_names.size());
        for (const SelectItem& item : stmt.items) {
            if (item.star) {
                for (const Value& v : *row) out.push_back(v);
                // A row stored before a later ALTER could be short; pad with NULL.
                for (std::size_t i = row->size(); i < table->columns().size(); ++i) {
                    out.push_back(Value::null());
                }
            } else {
                out.push_back(evaluate(*item.expr, table, row));
            }
        }
        result.rows.push_back(std::move(out));
    }

    return result;
}

}  // namespace minidb
