#include "minidb/database.hpp"

#include <utility>

#include "minidb/diagnostic.hpp"
#include "minidb/record.hpp"
#include "minidb/util.hpp"

namespace minidb {
namespace {

// A catalogue entry is just a row, so it reuses the ordinary record encoder:
//   name, root page, next rowid, then (column name, column type) pairs.
Row catalog_row(const Table& table) {
    Row row;
    row.push_back(Value::text(table.name()));
    row.push_back(Value::integer(static_cast<std::int64_t>(table.root())));
    row.push_back(Value::integer(table.next_rowid()));
    for (const ColumnDef& column : table.columns()) {
        row.push_back(Value::text(column.name));
        row.push_back(Value::integer(static_cast<std::int64_t>(column.type)));
    }
    return row;
}

}  // namespace

Database::Database(std::string path) : path_(std::move(path)) {
    pager_ = std::make_unique<Pager>(path_);

    if (pager_->catalog_root() == kInvalidPage) {
        pager_->set_catalog_root(BTree::create(*pager_));
    }
    catalog_ = std::make_unique<BTree>(*pager_, pager_->catalog_root());
    load_catalog();
}

Database::~Database() {
    try {
        flush();
    } catch (...) {
        // Destructors must not throw. Callers that care about durability call
        // flush() themselves and get the error there.
    }
}

void Database::load_catalog() {
    catalog_->scan([&](std::int64_t id, const std::uint8_t* payload, std::size_t size) {
        const Row row = decode_row(payload, size);
        if (row.size() < 3) throw DbError("corrupt catalogue entry");

        const std::string name = row[0].as_text();
        const PageId root = static_cast<PageId>(row[1].as_integer());
        const std::int64_t next_rowid = row[2].as_integer();

        std::vector<ColumnDef> columns;
        for (std::size_t i = 3; i + 1 < row.size(); i += 2) {
            ColumnDef column;
            column.name = row[i].as_text();
            const std::int64_t type = row[i + 1].as_integer();
            if (type < 0 || type > 3) throw DbError("corrupt catalogue: bad column type");
            column.type = static_cast<ValueType>(type);
            columns.push_back(std::move(column));
        }

        tables_.emplace(to_lower(name),
                        Table(name, std::move(columns), *pager_, root, next_rowid, id));
        return true;
    });
}

void Database::write_catalog_entry(const Table& table) {
    const std::vector<std::uint8_t> payload = encode_row(catalog_row(table));
    catalog_->insert(table.catalog_id(), payload.data(), payload.size());
    pager_->set_catalog_root(catalog_->root());  // the catalogue tree can grow too
}

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

    const PageId root = BTree::create(*pager_);
    const std::int64_t id = catalog_->max_key() + 1;

    auto result = tables_.emplace(
        key, Table(name, std::move(columns), *pager_, root, 1, id));
    Table& table = result.first->second;

    write_catalog_entry(table);
    table.mark_clean();
    return table;
}

std::vector<std::string> Database::table_names() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& entry : tables_) names.push_back(entry.second.name());
    return names;  // std::map iterates in sorted key order
}

void Database::flush() {
    if (!pager_) return;  // moved-from
    for (auto& entry : tables_) {
        if (entry.second.dirty()) {
            write_catalog_entry(entry.second);
            entry.second.mark_clean();
        }
    }
    pager_->flush();
}

}  // namespace minidb
