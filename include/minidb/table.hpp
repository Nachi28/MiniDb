#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "minidb/ast.hpp"
#include "minidb/btree.hpp"
#include "minidb/key.hpp"
#include "minidb/row.hpp"
#include "minidb/value.hpp"

namespace minidb {

// A table is a B+tree keyed by an implicit, monotonically increasing rowid,
// with the row's values encoded as the payload. Rows are decoded on the way out
// rather than being held in memory, so a table is only as resident as the
// buffer pool lets it be.
class Table {
public:
    Table(std::string name, std::vector<ColumnDef> columns, Pager& pager, PageId root,
          std::int64_t next_rowid, std::int64_t row_count, std::int64_t catalog_id);

    const std::string& name() const { return name_; }
    const std::vector<ColumnDef>& columns() const { return columns_; }

    // Case-insensitive lookup. Returns -1 when the column does not exist.
    int column_index(const std::string& column) const;

    // Stores a row and returns the rowid it was given.
    std::int64_t insert(Row row);

    // Visits rows in rowid order; return false from the callback to stop early.
    using Visitor = std::function<bool(std::int64_t, Row&)>;
    void scan(const Visitor& visit) const;

    // Fetches one row by rowid, which is how an index lookup lands on a row.
    bool find(std::int64_t rowid, Row* out) const;

    bool erase(std::int64_t rowid);

    std::size_t size() const;  // counted by scanning

    // Maintained incrementally and persisted in the catalogue, because the
    // planner needs a cardinality and cannot afford to count rows per query.
    std::int64_t row_count() const { return row_count_; }

    // Catalogue bookkeeping. The root moves whenever the tree gains a level, so
    // the owning Database has to notice and write the new value back.
    PageId root() const { return tree_.root(); }
    std::int64_t next_rowid() const { return next_rowid_; }
    std::int64_t catalog_id() const { return catalog_id_; }
    bool dirty() const { return dirty_; }
    void mark_clean() { dirty_ = false; }

private:
    std::string name_;
    std::vector<ColumnDef> columns_;
    BTree tree_;
    std::int64_t next_rowid_ = 1;
    std::int64_t row_count_ = 0;
    std::int64_t catalog_id_ = 0;
    bool dirty_ = false;
};

}  // namespace minidb
