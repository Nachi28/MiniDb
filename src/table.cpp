#include "minidb/table.hpp"

#include "minidb/util.hpp"

namespace minidb {

Table::Table(std::string name, std::vector<ColumnDef> columns)
    : name_(std::move(name)), columns_(std::move(columns)) {}

int Table::column_index(const std::string& column) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (iequals(columns_[i].name, column)) return static_cast<int>(i);
    }
    return -1;
}

void Table::insert(Row row) {
    row.resize(columns_.size());  // missing trailing columns default to NULL
    rows_.push_back(std::move(row));
}

}  // namespace minidb
