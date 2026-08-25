#include "minidb/database.hpp"

#include <utility>

#include "minidb/diagnostic.hpp"
#include "minidb/util.hpp"

namespace minidb {

Table* Database::find_table(const std::string& name) {
    const auto it = tables_.find(to_lower(name));
    return it == tables_.end() ? nullptr : &it->second;
}

const Table* Database::find_table(const std::string& name) const {
    const auto it = tables_.find(to_lower(name));
    return it == tables_.end() ? nullptr : &it->second;
}

Table& Database::create_table(const std::string& name, std::vector<ColumnDef> columns) {
    const std::string key = to_lower(name);
    if (tables_.find(key) != tables_.end()) {
        throw DbError("table " + name + " already exists");
    }
    auto result = tables_.emplace(key, Table(name, std::move(columns)));
    return result.first->second;
}

std::vector<std::string> Database::table_names() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& entry : tables_) names.push_back(entry.second.name());
    return names;  // std::map iterates in sorted key order
}

}  // namespace minidb
