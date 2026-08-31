#include "minidb/database.hpp"

#include <utility>

#include "minidb/diagnostic.hpp"
#include "minidb/key.hpp"
#include "minidb/record.hpp"
#include "minidb/util.hpp"

namespace minidb {
namespace {

// A catalogue entry is just a row, so it reuses the ordinary record encoder.
// The leading kind field says how to read the rest:
//   table: 0, name, root page, next rowid, then (column name, type) pairs
//   index: 1, name, root page, table name, column name
constexpr std::int64_t kKindTable = 0;
constexpr std::int64_t kKindIndex = 1;

Row catalog_row(const Table& table) {
    Row row;
    row.push_back(Value::integer(kKindTable));
    row.push_back(Value::text(table.name()));
    row.push_back(Value::integer(static_cast<std::int64_t>(table.root())));
    row.push_back(Value::integer(table.next_rowid()));
    row.push_back(Value::integer(table.row_count()));
    for (const ColumnDef& column : table.columns()) {
        row.push_back(Value::text(column.name));
        row.push_back(Value::integer(static_cast<std::int64_t>(column.type)));
    }
    return row;
}

Row catalog_row(const Index& index) {
    Row row;
    row.push_back(Value::integer(kKindIndex));
    row.push_back(Value::text(index.name()));
    row.push_back(Value::integer(static_cast<std::int64_t>(index.root())));
    row.push_back(Value::text(index.table()));
    row.push_back(Value::text(index.column()));
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
    catalog_->scan([&](Slice key, Slice payload) {
        const std::int64_t id = decode_rowid(key);
        const Row row = decode_row(payload.data, payload.size);
        if (row.size() < 4) throw DbError("corrupt catalogue entry");

        const std::int64_t kind = row[0].as_integer();
        const std::string name = row[1].as_text();
        const PageId root = static_cast<PageId>(row[2].as_integer());

        if (kind == kKindIndex) {
            if (row.size() < 5) throw DbError("corrupt catalogue index entry");
            indexes_.emplace(to_lower(name),
                             Index(name, row[3].as_text(), row[4].as_text(), *pager_,
                                   root, id));
            return true;
        }
        if (kind != kKindTable) throw DbError("corrupt catalogue: unknown entry kind");

        if (row.size() < 5) throw DbError("corrupt catalogue table entry");
        const std::int64_t next_rowid = row[3].as_integer();
        const std::int64_t row_count = row[4].as_integer();

        std::vector<ColumnDef> columns;
        for (std::size_t i = 5; i + 1 < row.size(); i += 2) {
            ColumnDef column;
            column.name = row[i].as_text();
            const std::int64_t type = row[i + 1].as_integer();
            if (type < 0 || type > 3) throw DbError("corrupt catalogue: bad column type");
            column.type = static_cast<ValueType>(type);
            columns.push_back(std::move(column));
        }

        tables_.emplace(to_lower(name),
                        Table(name, std::move(columns), *pager_, root, next_rowid,
                              row_count, id));
        return true;
    });
}

std::int64_t Database::next_catalog_id() const {
    const std::string largest = catalog_->max_key();
    return largest.empty() ? 1 : decode_rowid(Slice(largest)) + 1;
}

void Database::write_catalog_entry(const Index& index) {
    const std::vector<std::uint8_t> payload = encode_row(catalog_row(index));
    const std::string key = encode_rowid(index.catalog_id());
    catalog_->insert(Slice(key), Slice(payload.data(), payload.size()));
    pager_->set_catalog_root(catalog_->root());
}

void Database::write_catalog_entry(const Table& table) {
    const std::vector<std::uint8_t> payload = encode_row(catalog_row(table));
    const std::string key = encode_rowid(table.catalog_id());
    catalog_->insert(Slice(key), Slice(payload.data(), payload.size()));
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
    const std::int64_t id = next_catalog_id();

    auto result = tables_.emplace(
        key, Table(name, std::move(columns), *pager_, root, 1, 0, id));
    Table& table = result.first->second;

    write_catalog_entry(table);
    table.mark_clean();
    return table;
}

Index* Database::find_index(const std::string& name) {
    const auto it = indexes_.find(to_lower(name));
    return it == indexes_.end() ? nullptr : &it->second;
}

const Index* Database::find_index(const std::string& name) const {
    const auto it = indexes_.find(to_lower(name));
    return it == indexes_.end() ? nullptr : &it->second;
}

const Index* Database::index_for(const std::string& table,
                                 const std::string& column) const {
    for (const auto& entry : indexes_) {
        if (iequals(entry.second.table(), table) &&
            iequals(entry.second.column(), column)) {
            return &entry.second;
        }
    }
    return nullptr;
}

Index& Database::create_index(const std::string& name, const std::string& table_name,
                              const std::string& column) {
    const std::string key = to_lower(name);
    if (indexes_.find(key) != indexes_.end()) {
        throw DbError("index " + name + " already exists");
    }
    Table* table = find_table(table_name);
    if (table == nullptr) throw DbError("no such table: " + table_name);

    const int column_index = table->column_index(column);
    if (column_index < 0) {
        throw DbError("no such column: " + column + " in table " + table->name());
    }
    if (index_for(table->name(), column) != nullptr) {
        throw DbError("column " + column + " is already indexed");
    }

    const PageId root = BTree::create(*pager_);
    auto result = indexes_.emplace(
        key, Index(name, table->name(), table->columns()[static_cast<std::size_t>(
                                           column_index)].name,
                   *pager_, root, next_catalog_id()));
    Index& index = result.first->second;

    // Backfill from the rows already there, so an index can be added to a
    // populated table.
    const std::size_t position = static_cast<std::size_t>(column_index);
    table->scan([&](std::int64_t rowid, Row& row) {
        index.insert(position < row.size() ? row[position] : Value::null(), rowid);
        return true;
    });

    write_catalog_entry(index);
    index.mark_clean();
    return index;
}

std::int64_t Database::insert_row(Table& table, Row row) {
    row.resize(table.columns().size());
    const std::int64_t rowid = table.insert(row);

    for (auto& entry : indexes_) {
        Index& index = entry.second;
        if (!iequals(index.table(), table.name())) continue;
        const int position = table.column_index(index.column());
        if (position < 0) continue;
        index.insert(row[static_cast<std::size_t>(position)], rowid);
    }
    return rowid;
}

std::vector<std::string> Database::index_names() const {
    std::vector<std::string> names;
    names.reserve(indexes_.size());
    for (const auto& entry : indexes_) names.push_back(entry.second.name());
    return names;
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
    for (auto& entry : indexes_) {
        if (entry.second.dirty()) {
            write_catalog_entry(entry.second);
            entry.second.mark_clean();
        }
    }
    pager_->commit();
}

void Database::begin() {
    if (in_transaction_) throw DbError("a transaction is already open");
    flush();  // start from a clean, durable point
    in_transaction_ = true;
}

void Database::commit() {
    flush();
    in_transaction_ = false;
}

void Database::rollback() {
    if (!pager_) return;
    pager_->rollback();

    // In-memory schema state is now stale: a table's root page and rowid
    // counter may have moved inside the aborted transaction, and a table
    // created inside it no longer exists. Rebuild from what is durable.
    tables_.clear();
    indexes_.clear();
    catalog_ = std::make_unique<BTree>(*pager_, pager_->catalog_root());
    load_catalog();
    in_transaction_ = false;
}

void Database::autocommit() {
    if (!in_transaction_) flush();
}

}  // namespace minidb
