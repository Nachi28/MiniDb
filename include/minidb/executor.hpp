#pragma once

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

// Phase 1 executes the AST directly. Phase 5 will lower it to bytecode and run
// it on a VM; ResultSet is the boundary that stays fixed across that change.
class Executor {
public:
    explicit Executor(Database& db) : db_(db) {}

    ResultSet execute(const Statement& stmt);

private:
    ResultSet exec_create(const CreateTableStmt& stmt);
    ResultSet exec_insert(const InsertStmt& stmt);
    ResultSet exec_select(const SelectStmt& stmt);

    Database& db_;
};

// Evaluates an expression against one row. `table` and `row` may both be null
// for constant expressions (INSERT values, SELECT without FROM).
Value evaluate(const Expr& expr, const Table* table, const Row* row);

// SQL truthiness used by WHERE: NULL and non-numeric values are not true.
bool is_true(const Value& v);

}  // namespace minidb
