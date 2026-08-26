#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "minidb/page.hpp"
#include "minidb/pager.hpp"

namespace minidb {

// A B+tree keyed by int64, with variable-length payloads living in the leaves.
// Internal nodes carry separator keys only, and leaves are chained left to
// right, so a full table scan is a linked-list walk rather than a tree walk.
//
// Pages are slotted: a sorted array of 2-byte offsets grows forward from the
// node header while cell data grows backward from the end of the page, which is
// what lets rows of different sizes share a page without padding.
class BTree {
public:
    // A cell must leave room for at least two per leaf, otherwise a split could
    // not make progress. Larger rows need overflow pages, which phase 3 does
    // not have yet; they are rejected with a clear error instead.
    static constexpr std::size_t kMaxPayload = 1980;

    BTree(Pager& pager, PageId root);

    // Allocates an empty tree and returns its root page.
    static PageId create(Pager& pager);

    PageId root() const { return root_; }

    // Inserts, or replaces the payload if the key is already present.
    void insert(std::int64_t key, const std::uint8_t* payload, std::size_t size);

    bool find(std::int64_t key, std::vector<std::uint8_t>* out) const;

    bool erase(std::int64_t key);

    // Visits every entry in ascending key order; return false to stop early.
    using Visitor = std::function<bool(std::int64_t, const std::uint8_t*, std::size_t)>;
    void scan(const Visitor& visit) const;

    std::size_t size() const;      // entries, counted by scanning
    std::size_t height() const;    // 1 for a tree that is a single leaf
    std::int64_t max_key() const;  // 0 when empty

private:
    struct Split {
        bool happened = false;
        std::int64_t key = 0;            // separator promoted into the parent
        PageId right = kInvalidPage;     // page the separator points past
    };

    Split insert_into(PageId page, std::int64_t key, const std::uint8_t* payload,
                      std::size_t size);
    PageId leftmost_leaf() const;
    PageId find_leaf(std::int64_t key) const;

    Pager* pager_;
    PageId root_;
};

}  // namespace minidb
