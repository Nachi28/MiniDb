#include "minidb/index.hpp"

#include <utility>

namespace minidb {
namespace {

// The smallest rowid, so that (value, kLowestRowid) sorts at or before every
// entry carrying that value.
constexpr std::int64_t kLowestRowid = -9223372036854775807LL - 1;

}  // namespace

Index::Index(std::string name, std::string table, std::string column, Pager& pager,
             PageId root, std::int64_t catalog_id)
    : name_(std::move(name)),
      table_(std::move(table)),
      column_(std::move(column)),
      tree_(pager, root),
      catalog_id_(catalog_id) {}

void Index::insert(const Value& value, std::int64_t rowid) {
    const std::string key = encode_index_key(value, rowid);
    tree_.insert(Slice(key), Slice());
    dirty_ = true;
}

bool Index::erase(const Value& value, std::int64_t rowid) {
    const std::string key = encode_index_key(value, rowid);
    const bool removed = tree_.erase(Slice(key));
    if (removed) dirty_ = true;
    return removed;
}

void Index::seek_equal(const Value& value, const RowidVisitor& visit) const {
    const std::string start = encode_index_key(value, kLowestRowid);
    const std::string stop = encode_index_upper_bound(value);

    tree_.scan_from(Slice(start), [&](Slice key, Slice) {
        if (compare_slices(key, Slice(stop)) >= 0) return false;  // past this value
        return visit(index_key_rowid(key));
    });
}

std::size_t Index::count_equal(const Value& value, std::size_t cap) const {
    std::size_t n = 0;
    seek_equal(value, [&](std::int64_t) {
        ++n;
        return n < cap;
    });
    return n;
}

std::size_t Index::count_range(const Value* low, bool low_inclusive,
                               const Value* high, bool high_inclusive,
                               std::size_t cap) const {
    std::size_t n = 0;
    seek_range(low, low_inclusive, high, high_inclusive, [&](std::int64_t) {
        ++n;
        return n < cap;
    });
    return n;
}

void Index::seek_range(const Value* low, bool low_inclusive, const Value* high,
                       bool high_inclusive, const RowidVisitor& visit) const {
    std::string start;
    if (low != nullptr) {
        // An exclusive lower bound starts just past every entry for that value.
        start = low_inclusive ? encode_index_key(*low, kLowestRowid)
                              : encode_index_upper_bound(*low);
    }

    std::string stop;
    bool bounded = false;
    if (high != nullptr) {
        bounded = true;
        stop = high_inclusive ? encode_index_upper_bound(*high)
                              : encode_index_key(*high, kLowestRowid);
    }

    tree_.scan_from(Slice(start), [&](Slice key, Slice) {
        if (bounded && compare_slices(key, Slice(stop)) >= 0) return false;
        return visit(index_key_rowid(key));
    });
}

}  // namespace minidb
