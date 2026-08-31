#include "minidb/plan.hpp"

#include <cmath>
#include <sstream>
#include <vector>

#include "minidb/util.hpp"

namespace minidb {
namespace {

// Reaching a row through an index means a random descent of the table tree,
// while a scan step walks a leaf chain that is already in the buffer pool. Four
// is a rough ratio, but having one at all is what makes the planner prefer a
// scan for unselective predicates instead of always reaching for an index.
constexpr double kRandomFetchCost = 4.0;

// The break-even point: past this many matches an index costs more than just
// reading the table, so the probe below can stop counting there.
double break_even(std::int64_t rows) {
    const double cap = static_cast<double>(rows) / kRandomFetchCost;
    return cap < 1.0 ? 1.0 : cap;
}

struct Constraint {
    std::string column;
    BinaryOp op = BinaryOp::Eq;
    Value value;
};

// Collects the conjuncts of a WHERE clause. Only top-level ANDs are usable:
// under an OR a row failing one side may still qualify, so no single index
// range covers the predicate.
void collect_conjuncts(const Expr& expr, std::vector<const Expr*>& out) {
    if (expr.kind == ExprKind::Binary && expr.binary_op == BinaryOp::And) {
        collect_conjuncts(*expr.left, out);
        collect_conjuncts(*expr.right, out);
        return;
    }
    out.push_back(&expr);
}

BinaryOp mirror(BinaryOp op) {
    switch (op) {
        case BinaryOp::Lt: return BinaryOp::Gt;
        case BinaryOp::LtEq: return BinaryOp::GtEq;
        case BinaryOp::Gt: return BinaryOp::Lt;
        case BinaryOp::GtEq: return BinaryOp::LtEq;
        default: return op;
    }
}

bool is_lower_bound(BinaryOp op) {
    return op == BinaryOp::Gt || op == BinaryOp::GtEq;
}

bool is_upper_bound(BinaryOp op) {
    return op == BinaryOp::Lt || op == BinaryOp::LtEq;
}

// Recognises `column OP literal` and `literal OP column`, the shape an index
// can actually exploit.
bool as_constraint(const Expr& expr, Constraint* out) {
    if (expr.kind != ExprKind::Binary) return false;
    switch (expr.binary_op) {
        case BinaryOp::Eq:
        case BinaryOp::Lt:
        case BinaryOp::LtEq:
        case BinaryOp::Gt:
        case BinaryOp::GtEq:
            break;
        default:
            return false;
    }

    const Expr& lhs = *expr.left;
    const Expr& rhs = *expr.right;

    if (lhs.kind == ExprKind::Column && rhs.kind == ExprKind::Literal) {
        out->column = lhs.column;
        out->op = expr.binary_op;
        out->value = rhs.literal;
    } else if (lhs.kind == ExprKind::Literal && rhs.kind == ExprKind::Column) {
        out->column = rhs.column;
        out->op = mirror(expr.binary_op);
        out->value = lhs.literal;
    } else {
        return false;
    }

    // NULL never matches a comparison, so seeking for it would be pointless.
    return !out->value.is_null();
}

double index_cost(double matches, std::int64_t rows) {
    const double descend = std::log2(static_cast<double>(rows) + 2.0);
    return descend + matches * kRandomFetchCost;
}

}  // namespace

std::string Plan::describe() const {
    std::ostringstream out;
    switch (path) {
        case AccessPath::FullScan:
            out << "SCAN TABLE";
            break;
        case AccessPath::IndexEqual:
            out << "SEEK INDEX " << index->name() << " (" << index->column() << " = "
                << equal_value.to_sql() << ")";
            break;
        case AccessPath::IndexRange:
            out << "RANGE INDEX " << index->name() << " (";
            if (has_low) out << low.to_sql() << (low_inclusive ? " <= " : " < ");
            out << index->column();
            if (has_high) out << (high_inclusive ? " <= " : " < ") << high.to_sql();
            out << ")";
            break;
    }
    out << (exact_estimate ? "  rows=" : "  rows>=")
        << static_cast<std::int64_t>(estimated_rows + 0.5) << " of "
        << table_rows << "  cost~" << static_cast<std::int64_t>(estimated_cost + 0.5)
        << " (scan would be " << static_cast<std::int64_t>(scan_cost + 0.5) << ")";
    return out.str();
}

Plan choose_plan(const Database& db, const Table& table, const Expr* where) {
    Plan plan;
    plan.table_rows = table.row_count();
    plan.estimated_rows = static_cast<double>(plan.table_rows);
    plan.exact_estimate = true;  // a scan reads every row, by definition
    plan.scan_cost = static_cast<double>(plan.table_rows);
    plan.estimated_cost = plan.scan_cost;
    if (where == nullptr) return plan;

    std::vector<const Expr*> conjuncts;
    collect_conjuncts(*where, conjuncts);

    for (const Expr* conjunct : conjuncts) {
        Constraint constraint;
        if (!as_constraint(*conjunct, &constraint)) continue;

        const Index* index = db.index_for(table.name(), constraint.column);
        if (index == nullptr) continue;

        Plan candidate;
        candidate.index = index;
        candidate.table_rows = plan.table_rows;
        candidate.scan_cost = plan.scan_cost;

        const std::size_t cap = static_cast<std::size_t>(break_even(plan.table_rows)) + 1;

        if (constraint.op == BinaryOp::Eq) {
            candidate.path = AccessPath::IndexEqual;
            candidate.equal_value = constraint.value;
            // Counting the index entries is exact, and bounded by the point at
            // which the answer stops mattering.
            candidate.estimated_rows =
                static_cast<double>(index->count_equal(constraint.value, cap));
            candidate.exact_estimate = candidate.estimated_rows < static_cast<double>(cap);
        } else {
            candidate.path = AccessPath::IndexRange;
            if (is_lower_bound(constraint.op)) {
                candidate.has_low = true;
                candidate.low = constraint.value;
                candidate.low_inclusive = constraint.op == BinaryOp::GtEq;
            } else {
                candidate.has_high = true;
                candidate.high = constraint.value;
                candidate.high_inclusive = constraint.op == BinaryOp::LtEq;
            }

            // A second constraint on the same column closes the other end, which
            // both narrows the estimate and lowers the cost.
            for (const Expr* other : conjuncts) {
                if (other == conjunct) continue;
                Constraint extra;
                if (!as_constraint(*other, &extra)) continue;
                if (!iequals(extra.column, constraint.column)) continue;

                if (is_lower_bound(extra.op) && !candidate.has_low) {
                    candidate.has_low = true;
                    candidate.low = extra.value;
                    candidate.low_inclusive = extra.op == BinaryOp::GtEq;
                } else if (is_upper_bound(extra.op) && !candidate.has_high) {
                    candidate.has_high = true;
                    candidate.high = extra.value;
                    candidate.high_inclusive = extra.op == BinaryOp::LtEq;
                }
            }
            candidate.estimated_rows = static_cast<double>(index->count_range(
                candidate.has_low ? &candidate.low : nullptr, candidate.low_inclusive,
                candidate.has_high ? &candidate.high : nullptr,
                candidate.high_inclusive, cap));
            candidate.exact_estimate = candidate.estimated_rows < static_cast<double>(cap);
        }

        candidate.estimated_cost = index_cost(candidate.estimated_rows, plan.table_rows);

        if (candidate.estimated_cost < plan.estimated_cost) plan = candidate;
    }

    return plan;
}

}  // namespace minidb
