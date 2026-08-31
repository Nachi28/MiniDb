#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "minidb/btree.hpp"
#include "minidb/key.hpp"
#include "minidb/value.hpp"

namespace minidb {

// A secondary index: a B+tree keyed by (indexed value, rowid), with an empty
// payload because the rowid is already in the key.
//
// Pairing the value with the rowid is what lets duplicate values coexist while
// still sorting into one contiguous, seekable run per value -- so an equality
// lookup is a seek plus a walk, and a range is the same walk with a later stop.
class Index {
public:
    Index(std::string name, std::string table, std::string column, Pager& pager,
          PageId root, std::int64_t catalog_id);

    const std::string& name() const { return name_; }
    const std::string& table() const { return table_; }
    const std::string& column() const { return column_; }

    void insert(const Value& value, std::int64_t rowid);
    bool erase(const Value& value, std::int64_t rowid);

    using RowidVisitor = std::function<bool(std::int64_t)>;

    // Rowids whose indexed value equals `value`, in rowid order.
    void seek_equal(const Value& value, const RowidVisitor& visit) const;

    // Rowids within the given bounds; pass null for an open end.
    void seek_range(const Value* low, bool low_inclusive, const Value* high,
                    bool high_inclusive, const RowidVisitor& visit) const;

    // Counts matching entries, giving up once `cap` is reached. A result equal
    // to `cap` means "at least cap", which is all the planner needs to know to
    // reject the index -- so the probe costs at most as much as the answer is
    // worth.
    std::size_t count_range(const Value* low, bool low_inclusive, const Value* high,
                            bool high_inclusive, std::size_t cap) const;
    std::size_t count_equal(const Value& value, std::size_t cap) const;

    std::size_t size() const { return tree_.size(); }
    std::size_t height() const { return tree_.height(); }

    PageId root() const { return tree_.root(); }
    std::int64_t catalog_id() const { return catalog_id_; }
    bool dirty() const { return dirty_; }
    void mark_clean() { dirty_ = false; }

private:
    std::string name_;
    std::string table_;
    std::string column_;
    BTree tree_;
    std::int64_t catalog_id_ = 0;
    bool dirty_ = false;
};

}  // namespace minidb
