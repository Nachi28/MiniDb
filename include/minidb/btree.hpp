#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "minidb/page.hpp"
#include "minidb/pager.hpp"
#include "minidb/slice.hpp"

namespace minidb {

// A B+tree over byte-string keys, ordered by plain memcmp.
//
// One tree implementation serves both roles in the database because the key
// encodings do the work: a table tree is keyed by an order-preserving rowid,
// and an index tree by (column value, rowid). Internal nodes carry separator
// keys only, and leaves are chained left to right, so a full scan is a
// linked-list walk rather than a tree walk.
//
// Pages are slotted: a sorted array of 2-byte offsets grows forward from the
// node header while cell data grows backward from the end of the page, which is
// what lets keys and rows of different sizes share a page without padding.
class BTree {
public:
    // Two cells must always fit in a page, or a split could not make progress.
    static constexpr std::size_t kMaxKey = 300;
    static constexpr std::size_t kMaxPayload = 1700;

    BTree(Pager& pager, PageId root);

    // Allocates an empty tree and returns its root page.
    static PageId create(Pager& pager);

    PageId root() const { return root_; }

    // Inserts, or replaces the payload if the key is already present.
    void insert(Slice key, Slice payload);

    bool find(Slice key, std::vector<std::uint8_t>* out) const;

    bool erase(Slice key);

    // Visits entries in ascending key order; return false to stop early.
    using Visitor = std::function<bool(Slice key, Slice payload)>;
    void scan(const Visitor& visit) const;

    // Same, but starting at the first key >= `start`. This is what turns an
    // index into a range scan.
    void scan_from(Slice start, const Visitor& visit) const;

    std::size_t size() const;      // entries, counted by scanning
    std::size_t height() const;    // 1 for a tree that is a single leaf
    std::string max_key() const;   // empty when the tree is empty

private:
    struct Split {
        bool happened = false;
        std::string key;                 // separator promoted into the parent
        PageId right = kInvalidPage;     // page the separator points past
    };

    Split insert_into(PageId page, Slice key, Slice payload);
    PageId leftmost_leaf() const;
    PageId find_leaf(Slice key) const;

    Pager* pager_;
    PageId root_;
};

}  // namespace minidb
