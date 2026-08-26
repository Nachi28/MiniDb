#include "minidb/executor.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "minidb/diagnostic.hpp"
#include "minidb/util.hpp"

namespace minidb {
namespace {

Value eval_binary(const Expr& expr, const EvalContext& ctx);
Value eval_call(const Expr& expr, const EvalContext& ctx);

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
        case ExprKind::FunctionCall:
            for (const ExprPtr& arg : expr.args) validate_columns(*arg, table);
            return;
    }
}

}  // namespace

bool is_aggregate_name(const std::string& name) {
    const std::string lower = to_lower(name);
    return lower == "count" || lower == "sum" || lower == "avg" || lower == "min" ||
           lower == "max";
}

bool is_true(const Value& v) {
    if (v.is_null()) return false;      // NULL is unknown, and unknown is not true
    if (v.is_number()) return v.as_double() != 0.0;
    return false;                       // TEXT is never a condition on its own
}

Value evaluate(const Expr& expr, const Table* table, const Row* row) {
    const EvalContext ctx{table, row, nullptr};
    return evaluate(expr, ctx);
}

Value evaluate(const Expr& expr, const EvalContext& ctx) {
    switch (expr.kind) {
        case ExprKind::Literal:
            return expr.literal;

        case ExprKind::Column: {
            if (ctx.table == nullptr || ctx.row == nullptr) {
                throw DbError("no such column: " + expr.column);
            }
            const int index = ctx.table->column_index(expr.column);
            if (index < 0) {
                throw DbError("no such column: " + expr.column + " in table " +
                              ctx.table->name());
            }
            const std::size_t i = static_cast<std::size_t>(index);
            return i < ctx.row->size() ? (*ctx.row)[i] : Value::null();
        }

        case ExprKind::Unary: {
            const Value v = evaluate(*expr.left, ctx);
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
            return eval_binary(expr, ctx);

        case ExprKind::FunctionCall:
            return eval_call(expr, ctx);
    }
    throw DbError("internal error: unhandled expression kind");
}

namespace {

Value eval_binary(const Expr& expr, const EvalContext& ctx) {
    const BinaryOp op = expr.binary_op;

    // AND / OR use SQL three-valued logic, so they cannot be folded into the
    // generic path below: FALSE AND NULL is FALSE, but TRUE AND NULL is NULL.
    if (op == BinaryOp::And || op == BinaryOp::Or) {
        const Value lhs = evaluate(*expr.left, ctx);
        if (op == BinaryOp::And) {
            if (!lhs.is_null() && !is_true(lhs)) return Value::boolean(false);
            const Value rhs = evaluate(*expr.right, ctx);
            if (!rhs.is_null() && !is_true(rhs)) return Value::boolean(false);
            if (lhs.is_null() || rhs.is_null()) return Value::null();
            return Value::boolean(true);
        }
        if (!lhs.is_null() && is_true(lhs)) return Value::boolean(true);
        const Value rhs = evaluate(*expr.right, ctx);
        if (!rhs.is_null() && is_true(rhs)) return Value::boolean(true);
        if (lhs.is_null() || rhs.is_null()) return Value::null();
        return Value::boolean(false);
    }

    const Value lhs = evaluate(*expr.left, ctx);
    const Value rhs = evaluate(*expr.right, ctx);

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

namespace {

Value eval_call(const Expr& expr, const EvalContext& ctx) {
    const std::string name = to_lower(expr.function);

    // Aggregates are not evaluated here: their value was computed once per
    // group and looked up by call site, so the surrounding expression tree can
    // treat them as ordinary leaves.
    if (is_aggregate_name(name)) {
        if (ctx.aggregates == nullptr) {
            throw DbError("misuse of aggregate function " + expr.function + "()");
        }
        const auto it = ctx.aggregates->find(&expr);
        if (it == ctx.aggregates->end()) {
            throw DbError("internal error: " + expr.function + "() was never computed");
        }
        return it->second;
    }

    if (expr.star_arg) throw DbError(expr.function + "() does not accept '*'");

    std::vector<Value> args;
    args.reserve(expr.args.size());
    for (const ExprPtr& arg : expr.args) args.push_back(evaluate(*arg, ctx));

    const auto arity = [&](std::size_t lo, std::size_t hi) {
        if (args.size() < lo || args.size() > hi) {
            throw DbError(expr.function + "() takes " + std::to_string(lo) +
                          (lo == hi ? "" : " to " + std::to_string(hi)) +
                          " argument(s), got " + std::to_string(args.size()));
        }
    };
    const auto numeric = [&](const Value& v) -> double {
        if (!v.is_number()) throw DbError(expr.function + "() requires a numeric argument");
        return v.as_double();
    };

    if (name == "abs") {
        arity(1, 1);
        if (args[0].is_null()) return Value::null();
        if (args[0].type() == ValueType::Integer) {
            const std::int64_t v = args[0].as_integer();
            return Value::integer(v < 0 ? -v : v);
        }
        return Value::real(std::fabs(numeric(args[0])));
    }
    if (name == "length") {
        arity(1, 1);
        if (args[0].is_null()) return Value::null();
        return Value::integer(static_cast<std::int64_t>(args[0].to_string().size()));
    }
    if (name == "lower" || name == "upper") {
        arity(1, 1);
        if (args[0].is_null()) return Value::null();
        std::string text = args[0].to_string();
        for (char& c : text) {
            const unsigned char u = static_cast<unsigned char>(c);
            c = static_cast<char>(name == "lower" ? std::tolower(u) : std::toupper(u));
        }
        return Value::text(text);
    }
    if (name == "round") {
        arity(1, 2);
        if (args[0].is_null()) return Value::null();
        double digits = 0.0;
        if (args.size() == 2) {
            if (args[1].is_null()) return Value::null();
            digits = numeric(args[1]);
        }
        const double scale = std::pow(10.0, digits);
        return Value::real(std::round(numeric(args[0]) * scale) / scale);
    }
    if (name == "coalesce" || name == "ifnull") {
        if (name == "ifnull") arity(2, 2);
        else arity(2, args.size() < 2 ? 2 : args.size());
        for (const Value& v : args) {
            if (!v.is_null()) return v;
        }
        return Value::null();
    }

    throw DbError("no such function: " + expr.function + "()");
}

}  // namespace

namespace {

bool is_aggregate_call(const Expr& e) {
    return e.kind == ExprKind::FunctionCall && is_aggregate_name(e.function);
}

bool contains_aggregate(const Expr& e) {
    if (is_aggregate_call(e)) return true;
    switch (e.kind) {
        case ExprKind::Unary:
            return contains_aggregate(*e.left);
        case ExprKind::Binary:
            return contains_aggregate(*e.left) || contains_aggregate(*e.right);
        case ExprKind::FunctionCall:
            for (const ExprPtr& arg : e.args) {
                if (contains_aggregate(*arg)) return true;
            }
            return false;
        default:
            return false;
    }
}

// Gathers each aggregate call site, without descending into one: SUM(COUNT(x))
// is rejected separately rather than silently flattened.
void collect_aggregates(const Expr& e, std::vector<const Expr*>& out) {
    if (is_aggregate_call(e)) {
        out.push_back(&e);
        for (const ExprPtr& arg : e.args) {
            if (contains_aggregate(*arg)) {
                throw DbError("aggregate functions cannot be nested inside " + e.function +
                              "()");
            }
        }
        if (e.star_arg) {
            if (!iequals(e.function, "count")) {
                throw DbError(e.function + "(*) is not allowed; only COUNT(*) is");
            }
        } else if (e.args.size() != 1) {
            throw DbError(e.function + "() takes exactly one argument");
        }
        return;
    }
    switch (e.kind) {
        case ExprKind::Unary:
            collect_aggregates(*e.left, out);
            return;
        case ExprKind::Binary:
            collect_aggregates(*e.left, out);
            collect_aggregates(*e.right, out);
            return;
        case ExprKind::FunctionCall:
            for (const ExprPtr& arg : e.args) collect_aggregates(*arg, out);
            return;
        default:
            return;
    }
}

// Every column referenced outside an aggregate must be one of the grouping
// keys, otherwise the value would be arbitrary across the group.
void check_grouped(const Expr& e, const std::vector<std::string>& keys) {
    if (is_aggregate_call(e)) return;

    const std::string rendered = to_string(e);
    for (const std::string& key : keys) {
        if (rendered == key) return;
    }

    switch (e.kind) {
        case ExprKind::Column:
            throw DbError("column " + e.column +
                          " must appear in GROUP BY or be used in an aggregate function");
        case ExprKind::Unary:
            check_grouped(*e.left, keys);
            return;
        case ExprKind::Binary:
            check_grouped(*e.left, keys);
            check_grouped(*e.right, keys);
            return;
        case ExprKind::FunctionCall:
            for (const ExprPtr& arg : e.args) check_grouped(*arg, keys);
            return;
        default:
            return;
    }
}

// ORDER BY may name a select-list alias or a 1-based output position rather
// than an expression of its own. Both are resolved to the underlying
// expression up front, so every later stage sees a plain expression tree.
const Expr* resolve_order_term(const Expr& e, const std::vector<SelectItem>& items) {
    if (e.kind == ExprKind::Column) {
        for (const SelectItem& item : items) {
            if (!item.star && !item.alias.empty() && iequals(item.alias, e.column)) {
                return item.expr.get();
            }
        }
        return &e;
    }
    if (e.kind == ExprKind::Literal && e.literal.type() == ValueType::Integer) {
        const std::int64_t n = e.literal.as_integer();
        if (n >= 1 && static_cast<std::size_t>(n) <= items.size() &&
            !items[static_cast<std::size_t>(n) - 1].star) {
            return items[static_cast<std::size_t>(n) - 1].expr.get();
        }
        throw DbError("ORDER BY position " + std::to_string(n) + " is out of range");
    }
    return &e;
}

// GROUP BY treats NULLs as equal to each other, which '=' deliberately does not.
struct GroupKeyLess {
    bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
        const std::size_t n = a.size() < b.size() ? a.size() : b.size();
        for (std::size_t i = 0; i < n; ++i) {
            const bool an = a[i].is_null();
            const bool bn = b[i].is_null();
            if (an && bn) continue;
            if (an) return true;
            if (bn) return false;
            const int r = compare(a[i], b[i]).value_or(0);
            if (r != 0) return r < 0;
        }
        return a.size() < b.size();
    }
};

Value compute_aggregate(const Expr& call, const Table* table,
                        const std::vector<const Row*>& rows) {
    const std::string name = to_lower(call.function);

    if (name == "count" && call.star_arg) {
        return Value::integer(static_cast<std::int64_t>(rows.size()));
    }

    bool seen = false;
    bool all_integer = true;
    double total = 0.0;
    std::int64_t integer_total = 0;
    std::int64_t counted = 0;
    Value extreme;

    for (const Row* row : rows) {
        const Value v = evaluate(*call.args[0], EvalContext{table, row, nullptr});
        if (v.is_null()) continue;  // every aggregate but COUNT(*) skips NULLs

        if (name == "min" || name == "max") {
            if (!seen) {
                extreme = v;
            } else {
                const int r = compare(v, extreme).value_or(0);
                if ((name == "min" && r < 0) || (name == "max" && r > 0)) extreme = v;
            }
        } else if (name != "count") {
            if (!v.is_number()) throw DbError(call.function + "() requires numeric values");
            if (v.type() != ValueType::Integer) all_integer = false;
            total += v.as_double();
            if (all_integer) integer_total += v.as_integer();
        }
        ++counted;
        seen = true;
    }

    if (name == "count") return Value::integer(counted);
    if (!seen) return Value::null();  // SUM/AVG/MIN/MAX over no rows is NULL
    if (name == "min" || name == "max") return extreme;
    if (name == "avg") return Value::real(total / static_cast<double>(counted));
    return all_integer ? Value::integer(integer_total) : Value::real(total);
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
            if (contains_aggregate(*row_exprs[i])) {
                throw DbError("aggregate functions are not allowed in INSERT values");
            }
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

    std::vector<const Expr*> order_exprs;
    order_exprs.reserve(stmt.order_by.size());
    for (const OrderTerm& term : stmt.order_by) {
        order_exprs.push_back(resolve_order_term(*term.expr, stmt.items));
    }

    // 1. Name resolution, before any row is touched.
    for (const SelectItem& item : stmt.items) {
        if (!item.star) validate_columns(*item.expr, table);
    }
    if (stmt.where) validate_columns(*stmt.where, table);
    if (stmt.having) validate_columns(*stmt.having, table);
    for (const ExprPtr& key : stmt.group_by) validate_columns(*key, table);
    for (const Expr* term : order_exprs) validate_columns(*term, table);

    if (stmt.where && contains_aggregate(*stmt.where)) {
        throw DbError("aggregate functions are not allowed in WHERE; use HAVING");
    }
    for (const ExprPtr& key : stmt.group_by) {
        if (contains_aggregate(*key)) {
            throw DbError("aggregate functions are not allowed in GROUP BY");
        }
    }

    // 2. Decide whether this is a grouped query, and gather the call sites whose
    //    values have to be computed once per group.
    std::vector<const Expr*> aggregates;
    for (const SelectItem& item : stmt.items) {
        if (!item.star) collect_aggregates(*item.expr, aggregates);
    }
    if (stmt.having) collect_aggregates(*stmt.having, aggregates);
    for (const Expr* term : order_exprs) collect_aggregates(*term, aggregates);

    const bool grouped =
        !stmt.group_by.empty() || !aggregates.empty() || stmt.having != nullptr;

    if (grouped) {
        for (const SelectItem& item : stmt.items) {
            if (item.star) {
                throw DbError("SELECT * cannot be combined with GROUP BY or aggregates");
            }
        }
        std::vector<std::string> keys;
        keys.reserve(stmt.group_by.size());
        for (const ExprPtr& key : stmt.group_by) keys.push_back(to_string(*key));
        for (const SelectItem& item : stmt.items) check_grouped(*item.expr, keys);
        for (const Expr* term : order_exprs) check_grouped(*term, keys);
        if (stmt.having) check_grouped(*stmt.having, keys);
    }

    static const Row kNoRow;  // stands in for the single row of a FROM-less SELECT

    // 3. Scan and filter. Rows are decoded out of the B+tree one at a time and
    //    only the survivors are kept, so a selective WHERE never materialises
    //    the whole table. `matched` points into `scanned`, which is not touched
    //    again once built.
    std::vector<Row> scanned;
    std::vector<const Row*> matched;
    if (table != nullptr) {
        table->scan([&](std::int64_t, Row& row) {
            if (stmt.where && !is_true(evaluate(*stmt.where, table, &row))) return true;
            scanned.push_back(std::move(row));
            return true;
        });
        matched.reserve(scanned.size());
        for (const Row& row : scanned) matched.push_back(&row);
    } else if (!stmt.where || is_true(evaluate(*stmt.where, nullptr, nullptr))) {
        matched.push_back(&kNoRow);
    }

    // 4. Grouping. Every later stage works on OutputRows, so ORDER BY, LIMIT and
    //    projection are written once and serve both grouped and plain queries.
    struct OutputRow {
        const Row* row = nullptr;
        std::map<const Expr*, Value> aggregate_values;
    };
    std::vector<OutputRow> output;

    if (!grouped) {
        output.reserve(matched.size());
        for (const Row* row : matched) output.push_back(OutputRow{row, {}});
    } else {
        std::vector<std::vector<const Row*>> groups;
        std::vector<const Row*> representatives;

        if (stmt.group_by.empty()) {
            // Bare aggregates form a single group, which still produces one row
            // when the table is empty: COUNT(*) is 0, SUM(x) is NULL.
            groups.push_back(matched);
            representatives.push_back(matched.empty() ? &kNoRow : matched.front());
        } else {
            std::map<std::vector<Value>, std::size_t, GroupKeyLess> index;
            for (const Row* row : matched) {
                std::vector<Value> key;
                key.reserve(stmt.group_by.size());
                for (const ExprPtr& g : stmt.group_by) key.push_back(evaluate(*g, table, row));

                const auto it = index.find(key);
                if (it == index.end()) {
                    index.emplace(std::move(key), groups.size());
                    groups.push_back(std::vector<const Row*>{row});
                    representatives.push_back(row);  // groups keep first-seen order
                } else {
                    groups[it->second].push_back(row);
                }
            }
        }

        for (std::size_t i = 0; i < groups.size(); ++i) {
            OutputRow out;
            out.row = representatives[i];
            for (const Expr* call : aggregates) {
                out.aggregate_values[call] = compute_aggregate(*call, table, groups[i]);
            }
            if (stmt.having) {
                const EvalContext ctx{table, out.row, &out.aggregate_values};
                if (!is_true(evaluate(*stmt.having, ctx))) continue;
            }
            output.push_back(std::move(out));
        }
    }

    const auto context_for = [&](const OutputRow& out) {
        return EvalContext{table, out.row, grouped ? &out.aggregate_values : nullptr};
    };

    // 5. ORDER BY. stable_sort keeps insertion order for equal keys.
    if (!stmt.order_by.empty()) {
        std::stable_sort(output.begin(), output.end(),
                         [&](const OutputRow& a, const OutputRow& b) {
                             for (std::size_t i = 0; i < order_exprs.size(); ++i) {
                                 const Value va = evaluate(*order_exprs[i], context_for(a));
                                 const Value vb = evaluate(*order_exprs[i], context_for(b));
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
                                 if (c != 0) {
                                     return stmt.order_by[i].descending ? c > 0 : c < 0;
                                 }
                             }
                             return false;
                         });
    }

    // 6. LIMIT.
    if (stmt.limit >= 0) {
        const std::size_t limit = static_cast<std::size_t>(stmt.limit);
        if (limit < output.size()) output.resize(limit);
    }

    // 7. Projection.
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

    for (const OutputRow& out : output) {
        Row projected;
        projected.reserve(result.column_names.size());
        for (const SelectItem& item : stmt.items) {
            if (item.star) {
                for (const Value& v : *out.row) projected.push_back(v);
                for (std::size_t i = out.row->size(); i < table->columns().size(); ++i) {
                    projected.push_back(Value::null());
                }
            } else {
                projected.push_back(evaluate(*item.expr, context_for(out)));
            }
        }
        result.rows.push_back(std::move(projected));
    }

    return result;
}

}  // namespace minidb
