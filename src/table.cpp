#include "minidb/table.hpp"

#include <utility>

#include "minidb/key.hpp"
#include "minidb/record.hpp"
#include "minidb/util.hpp"

namespace minidb {

Table::Table(std::string name, std::vector<ColumnDef> columns, Pager& pager, PageId root,
             std::int64_t next_rowid, std::int64_t row_count, std::int64_t catalog_id)
    : name_(std::move(name)),
      columns_(std::move(columns)),
      tree_(pager, root),
      next_rowid_(next_rowid),
      row_count_(row_count),
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
    const std::string key = encode_rowid(rowid);
    tree_.insert(Slice(key), Slice(payload.data(), payload.size()));
    ++row_count_;
    dirty_ = true;  // next_rowid_, row_count_ and maybe the root have moved
    return rowid;
}

void Table::scan(const Visitor& visit) const {
    tree_.scan([&](Slice key, Slice payload) {
        Row row = decode_row(payload.data, payload.size);
        row.resize(columns_.size());
        return visit(decode_rowid(key), row);
    });
}

bool Table::find(std::int64_t rowid, Row* out) const {
    const std::string key = encode_rowid(rowid);
    std::vector<std::uint8_t> payload;
    if (!tree_.find(Slice(key), &payload)) return false;
    if (out != nullptr) {
        *out = decode_row(payload.data(), payload.size());
        out->resize(columns_.size());
    }
    return true;
}

bool Table::erase(std::int64_t rowid) {
    const std::string key = encode_rowid(rowid);
    const bool removed = tree_.erase(Slice(key));
    if (removed) {
        --row_count_;
        dirty_ = true;
    }
    return removed;
}

std::size_t Table::size() const { return tree_.size(); }

}  // namespace minidb
