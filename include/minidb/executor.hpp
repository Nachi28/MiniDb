#pragma once

#include <map>
#include <string>
#include <vector>

#include "minidb/ast.hpp"
#include "minidb/database.hpp"
#include "minidb/table.hpp"

namespace minidb {

struct ResultSet {
    bool is_query = false;                  // true for SELECT
    std::vector<std::string> column_names;
    std::vector<Row> rows;
    std::string message;                    // for CREATE / INSERT
};

// What an expression is evaluated against.
//
// `aggregates` maps each aggregate call site in the AST to the value already
// computed for the current group, so a projection like `SUM(x) / COUNT(*)`
// evaluates as an ordinary expression tree over precomputed leaves. It is null
// for non-grouped queries, which is also how `evaluate` detects an aggregate
// used somewhere it does not belong.
struct EvalContext {
    const Table* table = nullptr;
    const Row* row = nullptr;
    const std::map<const Expr*, Value>* aggregates = nullptr;
};

// Phase 2 executes the AST directly. Phase 5 will lower it to bytecode and run
// it on a VM; ResultSet is the boundary that stays fixed across that change.
class Executor {
public:
    explicit Executor(Database& db) : db_(db) {}

    ResultSet execute(const Statement& stmt);

private:
    ResultSet exec_create(const CreateTableStmt& stmt);
    ResultSet exec_insert(const InsertStmt& stmt);
    ResultSet exec_select(const SelectStmt& stmt);
    ResultSet exec_transaction(const TransactionStmt& stmt);

    Database& db_;
};

Value evaluate(const Expr& expr, const EvalContext& ctx);

// Convenience overload for constant and per-row evaluation with no aggregates.
Value evaluate(const Expr& expr, const Table* table, const Row* row);

// SQL truthiness used by WHERE and HAVING: NULL and non-numeric are not true.
bool is_true(const Value& v);

// True for COUNT / SUM / AVG / MIN / MAX, case-insensitively.
bool is_aggregate_name(const std::string& name);

}  // namespace minidb
