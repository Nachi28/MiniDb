#include "minidb/btree.hpp"

#include <algorithm>
#include <cstring>

#include "minidb/diagnostic.hpp"

namespace minidb {
namespace {

// Node header, at the start of every tree page.
//   0  uint8   node type
//   2  uint16  cell count
//   4  uint16  heap start (cells occupy [heap_start, kPageSize))
//   8  uint32  leaf: next leaf page;  internal: rightmost child
// followed by `cell count` 2-byte slot offsets, sorted by key.
//
// Leaf cell:      uint16 key length | key | uint16 payload length | payload
// Internal cell:  uint16 key length | key | uint32 child
constexpr std::size_t kNodeHeader = 12;
constexpr std::size_t kOffType = 0;
constexpr std::size_t kOffCellCount = 2;
constexpr std::size_t kOffHeapStart = 4;
constexpr std::size_t kOffExtra = 8;
constexpr std::uint8_t kLeaf = 1;
constexpr std::uint8_t kInternal = 2;

// A view over one page's bytes. Construct from mutable_data() to change the
// node, or from read_data() to read it.
class Node {
public:
    explicit Node(std::uint8_t* data) : data_(data) {}

    static void init(std::uint8_t* data, bool leaf) {
        std::fill(data, data + kPageSize, static_cast<std::uint8_t>(0));
        data[kOffType] = leaf ? kLeaf : kInternal;
        put_u16(data + kOffCellCount, 0);
        put_u16(data + kOffHeapStart, static_cast<std::uint16_t>(kPageSize));
        put_u32(data + kOffExtra, kInvalidPage);
    }

    bool is_leaf() const { return data_[kOffType] == kLeaf; }
    std::size_t count() const { return get_u16(data_ + kOffCellCount); }
    PageId extra() const { return get_u32(data_ + kOffExtra); }
    void set_extra(PageId id) { put_u32(data_ + kOffExtra, id); }

    const std::uint8_t* cell(std::size_t i) const { return data_ + get_slot(i); }
    std::uint8_t* cell(std::size_t i) { return data_ + get_slot(i); }

    Slice key_at(std::size_t i) const {
        const std::uint8_t* c = cell(i);
        return Slice(c + 2, get_u16(c));
    }

    Slice payload_at(std::size_t i) const {
        const std::uint8_t* c = cell(i);
        const std::size_t key_len = get_u16(c);
        return Slice(c + 2 + key_len + 2, get_u16(c + 2 + key_len));
    }

    PageId child_at(std::size_t i) const {
        const std::uint8_t* c = cell(i);
        return get_u32(c + 2 + get_u16(c));
    }

    std::size_t cell_size(std::size_t i) const {
        const std::uint8_t* c = cell(i);
        const std::size_t key_len = get_u16(c);
        if (!is_leaf()) return 2 + key_len + 4;
        return 2 + key_len + 2 + get_u16(c + 2 + key_len);
    }

    // First index whose key is >= `key`: where an equal key lives, or where a
    // new one belongs.
    std::size_t lower_bound(Slice key) const {
        std::size_t lo = 0;
        std::size_t hi = count();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (compare_slices(key_at(mid), key) < 0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    // First index whose key is strictly greater. Used to pick a child: cell i
    // covers keys < key_at(i), so a key equal to a separator lives to its right.
    std::size_t upper_bound(Slice key) const {
        std::size_t lo = 0;
        std::size_t hi = count();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (compare_slices(key_at(mid), key) <= 0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    std::size_t slots_end() const { return kNodeHeader + 2 * count(); }
    std::size_t heap_start() const { return get_u16(data_ + kOffHeapStart); }
    std::size_t contiguous_free() const { return heap_start() - slots_end(); }

    std::size_t total_free() const {
        std::size_t used = 0;
        for (std::size_t i = 0; i < count(); ++i) used += cell_size(i);
        return kPageSize - slots_end() - used;
    }

    // Rewrites the heap so the space left by removed cells becomes usable again.
    void compact() {
        std::vector<std::uint8_t> buffer;
        std::vector<std::size_t> sizes(count());
        for (std::size_t i = 0; i < count(); ++i) {
            sizes[i] = cell_size(i);
            const std::uint8_t* c = cell(i);
            buffer.insert(buffer.end(), c, c + sizes[i]);
        }
        std::size_t heap = kPageSize;
        std::size_t at = 0;
        for (std::size_t i = 0; i < count(); ++i) {
            heap -= sizes[i];
            std::memcpy(data_ + heap, buffer.data() + at, sizes[i]);
            set_slot(i, static_cast<std::uint16_t>(heap));
            at += sizes[i];
        }
        put_u16(data_ + kOffHeapStart, static_cast<std::uint16_t>(heap));
    }

    // Inserts `bytes` as cell number `index`, shifting later slots right.
    // False when the node is genuinely full.
    bool insert_cell(std::size_t index, const std::uint8_t* bytes, std::size_t size) {
        if (contiguous_free() < size + 2) {
            if (total_free() < size + 2) return false;
            compact();
        }
        const std::size_t heap = heap_start() - size;
        std::memcpy(data_ + heap, bytes, size);
        put_u16(data_ + kOffHeapStart, static_cast<std::uint16_t>(heap));

        const std::size_t n = count();
        for (std::size_t i = n; i > index; --i) set_slot(i, get_slot(i - 1));
        set_slot(index, static_cast<std::uint16_t>(heap));
        put_u16(data_ + kOffCellCount, static_cast<std::uint16_t>(n + 1));
        return true;
    }

    // Drops a slot. The cell's bytes stay behind as a hole until compact().
    void remove_cell(std::size_t index) {
        const std::size_t n = count();
        for (std::size_t i = index; i + 1 < n; ++i) set_slot(i, get_slot(i + 1));
        put_u16(data_ + kOffCellCount, static_cast<std::uint16_t>(n - 1));
    }

    std::uint16_t get_slot(std::size_t i) const {
        return get_u16(data_ + kNodeHeader + 2 * i);
    }
    void set_slot(std::size_t i, std::uint16_t offset) {
        put_u16(data_ + kNodeHeader + 2 * i, offset);
    }

private:
    std::uint8_t* data_;
};

std::vector<std::uint8_t> make_leaf_cell(Slice key, Slice payload) {
    std::vector<std::uint8_t> cell(2 + key.size + 2 + payload.size);
    put_u16(cell.data(), static_cast<std::uint16_t>(key.size));
    if (key.size > 0) std::memcpy(cell.data() + 2, key.data, key.size);
    put_u16(cell.data() + 2 + key.size, static_cast<std::uint16_t>(payload.size));
    if (payload.size > 0) {
        std::memcpy(cell.data() + 2 + key.size + 2, payload.data, payload.size);
    }
    return cell;
}

std::vector<std::uint8_t> make_internal_cell(Slice key, PageId child) {
    std::vector<std::uint8_t> cell(2 + key.size + 4);
    put_u16(cell.data(), static_cast<std::uint16_t>(key.size));
    if (key.size > 0) std::memcpy(cell.data() + 2, key.data, key.size);
    put_u32(cell.data() + 2 + key.size, child);
    return cell;
}

Slice slice_of(const std::string& s) { return Slice(s); }

}  // namespace

PageId BTree::create(Pager& pager) {
    PageId id = kInvalidPage;
    PageGuard guard = pager.allocate(&id);
    Node::init(guard.mutable_data(), /*leaf=*/true);
    return id;
}

BTree::BTree(Pager& pager, PageId root) : pager_(&pager), root_(root) {
    if (root_ == kInvalidPage) throw DbError("B-tree root page is not set");
}

PageId BTree::find_leaf(Slice key) const {
    PageId current = root_;
    for (;;) {
        PageGuard guard = pager_->fetch(current);
        const Node node(guard.read_data());
        if (node.is_leaf()) return current;
        const std::size_t index = node.upper_bound(key);
        current = index < node.count() ? node.child_at(index) : node.extra();
    }
}

PageId BTree::leftmost_leaf() const {
    PageId current = root_;
    for (;;) {
        PageGuard guard = pager_->fetch(current);
        const Node node(guard.read_data());
        if (node.is_leaf()) return current;
        current = node.count() > 0 ? node.child_at(0) : node.extra();
    }
}

bool BTree::find(Slice key, std::vector<std::uint8_t>* out) const {
    PageGuard guard = pager_->fetch(find_leaf(key));
    const Node leaf(guard.read_data());
    const std::size_t index = leaf.lower_bound(key);
    if (index >= leaf.count() || compare_slices(leaf.key_at(index), key) != 0) return false;
    if (out != nullptr) {
        const Slice payload = leaf.payload_at(index);
        out->assign(payload.data, payload.data + payload.size);
    }
    return true;
}

bool BTree::erase(Slice key) {
    PageGuard guard = pager_->fetch(find_leaf(key));
    Node leaf(guard.read_data());
    const std::size_t index = leaf.lower_bound(key);
    if (index >= leaf.count() || compare_slices(leaf.key_at(index), key) != 0) return false;
    Node(guard.mutable_data()).remove_cell(index);
    return true;
}

void BTree::scan(const Visitor& visit) const {
    PageId current = leftmost_leaf();
    while (current != kInvalidPage) {
        PageGuard guard = pager_->fetch(current);
        const Node leaf(guard.read_data());
        for (std::size_t i = 0; i < leaf.count(); ++i) {
            if (!visit(leaf.key_at(i), leaf.payload_at(i))) return;
        }
        current = leaf.extra();
    }
}

// Seeking to the first key >= start and then following the leaf chain is what
// makes an index range scan cost only the rows it actually returns.
void BTree::scan_from(Slice start, const Visitor& visit) const {
    PageId current = find_leaf(start);
    bool first = true;
    while (current != kInvalidPage) {
        PageGuard guard = pager_->fetch(current);
        const Node leaf(guard.read_data());
        std::size_t i = 0;
        if (first) {
            i = leaf.lower_bound(start);
            first = false;
        }
        for (; i < leaf.count(); ++i) {
            if (!visit(leaf.key_at(i), leaf.payload_at(i))) return;
        }
        current = leaf.extra();
    }
}

std::size_t BTree::size() const {
    std::size_t n = 0;
    scan([&](Slice, Slice) {
        ++n;
        return true;
    });
    return n;
}

std::size_t BTree::height() const {
    std::size_t levels = 1;
    PageId current = root_;
    for (;;) {
        PageGuard guard = pager_->fetch(current);
        const Node node(guard.read_data());
        if (node.is_leaf()) return levels;
        ++levels;
        current = node.count() > 0 ? node.child_at(0) : node.extra();
    }
}

std::string BTree::max_key() const {
    std::string largest;
    scan([&](Slice key, Slice) {
        largest = key.to_string();  // ascending order, so the last one wins
        return true;
    });
    return largest;
}

void BTree::insert(Slice key, Slice payload) {
    if (key.size > kMaxKey) {
        throw DbError("key is too large to index: " + std::to_string(key.size) +
                      " bytes, limit is " + std::to_string(kMaxKey));
    }
    if (payload.size > kMaxPayload) {
        throw DbError("row is too large to store: " + std::to_string(payload.size) +
                      " bytes, limit is " + std::to_string(kMaxPayload));
    }

    const Split split = insert_into(root_, key, payload);
    if (!split.happened) return;

    // The root split, so the tree gains a level. This is the only way a B+tree
    // grows in height, which is why every leaf stays the same distance away.
    PageId new_root = kInvalidPage;
    PageGuard guard = pager_->allocate(&new_root);
    Node::init(guard.mutable_data(), /*leaf=*/false);
    Node node(guard.mutable_data());
    const std::vector<std::uint8_t> cell = make_internal_cell(slice_of(split.key), root_);
    node.insert_cell(0, cell.data(), cell.size());
    node.set_extra(split.right);
    root_ = new_root;
}

BTree::Split BTree::insert_into(PageId page_id, Slice key, Slice payload) {
    PageGuard guard = pager_->fetch(page_id);
    const std::size_t capacity = kPageSize - kNodeHeader;

    if (Node(guard.read_data()).is_leaf()) {
        Node node(guard.mutable_data());
        const std::size_t index = node.lower_bound(key);
        if (index < node.count() && compare_slices(node.key_at(index), key) == 0) {
            node.remove_cell(index);  // upsert: the old payload goes first
        }

        const std::vector<std::uint8_t> cell = make_leaf_cell(key, payload);
        if (node.insert_cell(index, cell.data(), cell.size())) return Split{};

        // The leaf is full. Rebuild its contents plus the new entry, then divide
        // them between two pages.
        struct Entry {
            std::string key;
            std::vector<std::uint8_t> cell;
        };
        std::vector<Entry> entries;
        entries.reserve(node.count() + 1);
        for (std::size_t i = 0; i < node.count(); ++i) {
            const std::uint8_t* c = node.cell(i);
            entries.push_back(Entry{node.key_at(i).to_string(),
                                    std::vector<std::uint8_t>(c, c + node.cell_size(i))});
        }
        entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(index),
                       Entry{key.to_string(), cell});

        std::vector<std::size_t> sizes(entries.size());
        std::size_t total = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            sizes[i] = entries[i].cell.size() + 2;  // the cell plus its slot
            total += sizes[i];
        }

        std::size_t at = 0;
        if (index + 1 == entries.size()) {
            // The new key is the largest, so this is an append -- the usual shape
            // for rowid inserts. A balanced split would leave every leaf
            // permanently half empty, so give the new entry a page of its own
            // and leave the full page full.
            at = entries.size() - 1;
        } else if (index == 0) {
            at = 1;  // mirror image, for keys arriving in descending order
        } else {
            std::size_t best_gap = 0;
            std::size_t prefix = 0;
            for (std::size_t m = 1; m < entries.size(); ++m) {
                prefix += sizes[m - 1];
                const std::size_t suffix = total - prefix;
                if (prefix > capacity || suffix > capacity) continue;
                const std::size_t gap =
                    prefix > suffix ? prefix - suffix : suffix - prefix;
                if (at == 0 || gap < best_gap) {
                    at = m;
                    best_gap = gap;
                }
            }
        }
        if (at == 0 || at >= entries.size()) {
            throw DbError("internal error: no valid leaf split point");
        }

        const PageId old_next = node.extra();

        PageId right_id = kInvalidPage;
        PageGuard right_guard = pager_->allocate(&right_id);
        Node::init(right_guard.mutable_data(), /*leaf=*/true);
        Node right(right_guard.mutable_data());

        Node::init(guard.mutable_data(), /*leaf=*/true);
        Node left(guard.mutable_data());

        for (std::size_t i = 0; i < at; ++i) {
            left.insert_cell(i, entries[i].cell.data(), entries[i].cell.size());
        }
        for (std::size_t i = at; i < entries.size(); ++i) {
            right.insert_cell(i - at, entries[i].cell.data(), entries[i].cell.size());
        }
        left.set_extra(right_id);
        right.set_extra(old_next);

        return Split{true, entries[at].key, right_id};
    }

    // Internal node: descend, then absorb whatever the child reports.
    Node node(guard.read_data());
    const std::size_t index = node.upper_bound(key);
    const PageId child = index < node.count() ? node.child_at(index) : node.extra();

    const Split below = insert_into(child, key, payload);
    if (!below.happened) return Split{};

    struct Separator {
        std::string key;
        PageId child;
    };
    std::vector<Separator> seps;
    seps.reserve(node.count() + 1);
    for (std::size_t i = 0; i < node.count(); ++i) {
        seps.push_back(Separator{node.key_at(i).to_string(), node.child_at(i)});
    }
    PageId rightmost = node.extra();

    if (index < seps.size()) {
        // The old cell at `index` pointed at `child`; it now covers the right
        // half, and a new separator in front of it covers the left half.
        seps.insert(seps.begin() + static_cast<std::ptrdiff_t>(index),
                    Separator{below.key, child});
        seps[index + 1].child = below.right;
    } else {
        seps.push_back(Separator{below.key, child});
        rightmost = below.right;
    }

    const auto write_separators = [](Node& target, const std::vector<Separator>& list,
                                     std::size_t from, std::size_t to, PageId last) {
        for (std::size_t i = from; i < to; ++i) {
            const std::vector<std::uint8_t> c =
                make_internal_cell(slice_of(list[i].key), list[i].child);
            target.insert_cell(i - from, c.data(), c.size());
        }
        target.set_extra(last);
    };

    std::size_t needed = 0;
    for (const Separator& sep : seps) needed += 2 + sep.key.size() + 4 + 2;

    if (needed <= capacity) {
        Node::init(guard.mutable_data(), /*leaf=*/false);
        Node rebuilt(guard.mutable_data());
        write_separators(rebuilt, seps, 0, seps.size(), rightmost);
        return Split{};
    }

    // Split the internal node. Unlike a leaf split the middle separator moves
    // up rather than being copied, because internal keys are only signposts.
    const std::size_t mid = seps.size() / 2;
    const std::string promoted = seps[mid].key;

    PageId right_id = kInvalidPage;
    PageGuard right_guard = pager_->allocate(&right_id);
    Node::init(right_guard.mutable_data(), /*leaf=*/false);
    Node right(right_guard.mutable_data());
    write_separators(right, seps, mid + 1, seps.size(), rightmost);

    Node::init(guard.mutable_data(), /*leaf=*/false);
    Node left(guard.mutable_data());
    write_separators(left, seps, 0, mid, seps[mid].child);

    return Split{true, promoted, right_id};
}

}  // namespace minidb
