#pragma once

#include <map>
#include <string>
#include <vector>

#include "minidb/ast.hpp"
#include "minidb/table.hpp"

namespace minidb {

// A catalogue of tables. Names are case-insensitive; the original spelling is
// preserved on the Table itself for display.
class Database {
public:
    Table* find_table(const std::string& name);
    const Table* find_table(const std::string& name) const;

    Table& create_table(const std::string& name, std::vector<ColumnDef> columns);

    std::vector<std::string> table_names() const;  // sorted
    std::size_t table_count() const { return tables_.size(); }

private:
    std::map<std::string, Table> tables_;  // key: lowercased name
};

}  // namespace minidb
