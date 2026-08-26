#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "minidb/ast.hpp"
#include "minidb/btree.hpp"
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

    std::vector<std::string> table_names() const;  // sorted
    std::size_t table_count() const { return tables_.size(); }

    // Writes back any catalogue entries whose root page or rowid counter moved,
    // then flushes the buffer pool.
    void flush();

    const std::string& path() const { return path_; }
    Pager& pager() { return *pager_; }

private:
    void load_catalog();
    void write_catalog_entry(const Table& table);

    std::string path_;
    // Declared before the tables so that it outlives them: every Table holds a
    // pointer into this pager.
    std::unique_ptr<Pager> pager_;
    std::unique_ptr<BTree> catalog_;
    std::map<std::string, Table> tables_;  // key: lowercased name
};

}  // namespace minidb
