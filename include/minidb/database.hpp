#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "minidb/ast.hpp"
#include "minidb/btree.hpp"
#include "minidb/index.hpp"
#include "minidb/pager.hpp"
#include "minidb/table.hpp"

namespace minidb {

// A database file: a buffer pool over the file, a catalogue B+tree describing
// the tables, and one B+tree per table.
//
// The catalogue lives in its own tree rooted at a page recorded in the file
// header, so opening a database is: read the header, walk the catalogue, and
// the schema is back.
class Database {
public:
    static constexpr const char* kMemory = ":memory:";

    explicit Database(std::string path = kMemory);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = default;
    Database& operator=(Database&&) = default;

    Table* find_table(const std::string& name);
    const Table* find_table(const std::string& name) const;

    Table& create_table(const std::string& name, std::vector<ColumnDef> columns);

    // Inserts a row and keeps every index on that table in step. Callers go
    // through here rather than Table::insert so an index cannot fall behind.
    std::int64_t insert_row(Table& table, Row row);

    Index* find_index(const std::string& name);
    const Index* find_index(const std::string& name) const;

    // Builds an index and backfills it from the rows already in the table.
    Index& create_index(const std::string& name, const std::string& table,
                        const std::string& column);

    // The index on this exact column, if one exists.
    const Index* index_for(const std::string& table, const std::string& column) const;

    std::vector<std::string> index_names() const;  // sorted
    std::size_t index_count() const { return indexes_.size(); }

    std::vector<std::string> table_names() const;  // sorted
    std::size_t table_count() const { return tables_.size(); }

    // Writes back any catalogue entries whose root page or rowid counter moved,
    // then commits the buffer pool to the write-ahead log.
    void flush();

    // Explicit transactions. Outside one, every mutating statement commits by
    // itself, so a crash loses at most the statement that was in flight.
    void begin();
    void commit();
    void rollback();
    void autocommit();  // commits unless an explicit transaction is open
    bool in_transaction() const { return in_transaction_; }

    const std::string& path() const { return path_; }
    Pager& pager() { return *pager_; }

private:
    void load_catalog();
    void write_catalog_entry(const Table& table);
    void write_catalog_entry(const Index& index);
    std::int64_t next_catalog_id() const;

    std::string path_;
    // Declared before the tables so that it outlives them: every Table holds a
    // pointer into this pager.
    std::unique_ptr<Pager> pager_;
    std::unique_ptr<BTree> catalog_;
    std::map<std::string, Table> tables_;   // key: lowercased name
    std::map<std::string, Index> indexes_;  // key: lowercased name
    bool in_transaction_ = false;
};

}  // namespace minidb
