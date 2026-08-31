#pragma once

#include <cstdint>
#include <string>

#include "minidb/ast.hpp"
#include "minidb/database.hpp"
#include "minidb/index.hpp"
#include "minidb/table.hpp"

namespace minidb {

enum class AccessPath { FullScan, IndexEqual, IndexRange };

// How the executor should get at the rows of one table.
//
// An index is not automatically better than a scan: reaching a row through an
// index costs a random fetch, while a scan walks the leaf chain sequentially.
// The planner compares estimated costs and quite often picks the scan.
struct Plan {
    AccessPath path = AccessPath::FullScan;
    const Index* index = nullptr;

    Value equal_value;      // IndexEqual
    bool has_low = false;   // IndexRange
    bool low_inclusive = false;
    Value low;
    bool has_high = false;
    bool high_inclusive = false;
    Value high;

    std::int64_t table_rows = 0;
    double estimated_rows = 0.0;
    // True when the row count was counted rather than bounded: the probe
    // stopped early only if the index was already losing.
    bool exact_estimate = false;
    double estimated_cost = 0.0;
    double scan_cost = 0.0;

    std::string describe() const;
};

// Chooses an access path for `table` given the WHERE clause (which may be null).
Plan choose_plan(const Database& db, const Table& table, const Expr* where);

}  // namespace minidb
