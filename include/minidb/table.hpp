#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "minidb/ast.hpp"
#include "minidb/value.hpp"

namespace minidb {

using Row = std::vector<Value>;

// Phase 1 storage: an in-memory vector of rows.
// Phase 3 replaces the backing store with a paged B+tree; this interface is
// deliberately narrow so that swap stays local.
class Table {
public:
    Table(std::string name, std::vector<ColumnDef> columns);

    const std::string& name() const { return name_; }
    const std::vector<ColumnDef>& columns() const { return columns_; }

    // Case-insensitive lookup. Returns -1 when the column does not exist.
    int column_index(const std::string& column) const;

    void insert(Row row);
    const std::vector<Row>& rows() const { return rows_; }
    std::size_t size() const { return rows_.size(); }

private:
    std::string name_;
    std::vector<ColumnDef> columns_;
    std::vector<Row> rows_;
};

}  // namespace minidb
