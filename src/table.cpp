#include "minidb/table.hpp"

#include <utility>

#include "minidb/record.hpp"
#include "minidb/util.hpp"

namespace minidb {

Table::Table(std::string name, std::vector<ColumnDef> columns, Pager& pager, PageId root,
             std::int64_t next_rowid, std::int64_t catalog_id)
    : name_(std::move(name)),
      columns_(std::move(columns)),
      tree_(pager, root),
      next_rowid_(next_rowid),
      catalog_id_(catalog_id) {}

int Table::column_index(const std::string& column) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (iequals(columns_[i].name, column)) return static_cast<int>(i);
    }
    return -1;
}

std::int64_t Table::insert(Row row) {
    row.resize(columns_.size());  // missing trailing columns default to NULL

    const std::vector<std::uint8_t> payload = encode_row(row);
    const std::int64_t rowid = next_rowid_++;
    tree_.insert(rowid, payload.data(), payload.size());
    dirty_ = true;  // both next_rowid_ and possibly the tree root have moved
    return rowid;
}

void Table::scan(const Visitor& visit) const {
    tree_.scan([&](std::int64_t rowid, const std::uint8_t* payload, std::size_t size) {
        Row row = decode_row(payload, size);
        row.resize(columns_.size());
        return visit(rowid, row);
    });
}

bool Table::erase(std::int64_t rowid) {
    const bool removed = tree_.erase(rowid);
    if (removed) dirty_ = true;
    return removed;
}

std::size_t Table::size() const { return tree_.size(); }

}  // namespace minidb
