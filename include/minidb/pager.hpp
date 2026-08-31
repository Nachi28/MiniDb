#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "minidb/page.hpp"
#include "minidb/wal.hpp"

namespace minidb {

class Pager;

// An RAII pin on a buffered page. While a guard is alive the page cannot be
// evicted, so the pointer it hands out stays valid. Asking for mutable_data()
// marks the page dirty, so callers cannot forget to.
class PageGuard {
public:
    PageGuard() = default;
    PageGuard(Pager* pager, PageId id, std::uint8_t* data);
    ~PageGuard();

    PageGuard(PageGuard&& other) noexcept;
    PageGuard& operator=(PageGuard&& other) noexcept;
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    bool valid() const { return data_ != nullptr; }
    PageId id() const { return id_; }
    const std::uint8_t* data() const { return data_; }

    // Read access for code that builds a mutable view over the bytes but is
    // only reading them. Unlike mutable_data() it does not mark the page
    // dirty, so it must never be used to modify the page.
    std::uint8_t* read_data() const { return data_; }
    std::uint8_t* mutable_data();
    void release();

private:
    Pager* pager_ = nullptr;
    PageId id_ = kInvalidPage;
    std::uint8_t* data_ = nullptr;
    bool dirty_ = false;
};

// A fixed-size buffer pool over a paged file, with LRU eviction of unpinned
// pages and write-back on eviction. Pass ":memory:" for a database that never
// touches disk; the pool then grows on demand and never evicts.
class Pager {
public:
    static constexpr const char* kMemoryPath = ":memory:";

    explicit Pager(std::string path, std::size_t pool_pages = 64);
    ~Pager();

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    PageGuard fetch(PageId id);
    PageGuard allocate(PageId* out_id);

    // Appends every dirty page to the log, then a commit marker, then syncs.
    // Once this returns the changes survive a crash.
    void commit();

    // Throws away everything written since the last commit: the log tail goes,
    // and the buffer pool is purged so the next read comes from durable state.
    void rollback();

    // Folds committed log frames back into the database file and empties the
    // log. Runs automatically once the log grows past a threshold.
    void checkpoint();

    void flush() { commit(); }  // the older name for commit()

    PageId page_count() const { return page_count_; }
    bool memory_only() const { return memory_only_; }

    // A slot in the file header owned by the layer above, used to find the
    // catalogue after reopening the file.
    PageId catalog_root() const { return catalog_root_; }
    void set_catalog_root(PageId id);

    std::size_t cache_hits() const { return hits_; }
    std::size_t cache_misses() const { return misses_; }
    std::size_t disk_reads() const { return reads_; }
    std::size_t disk_writes() const { return writes_; }
    std::size_t evictions() const { return evictions_; }
    std::size_t wal_frames() const { return wal_ ? wal_->frame_count() : 0; }
    std::size_t checkpoints() const { return checkpoints_; }

private:
    friend class PageGuard;

    struct Frame {
        PageId id = kInvalidPage;
        std::vector<std::uint8_t> data = std::vector<std::uint8_t>(kPageSize, 0);
        bool dirty = false;
        int pins = 0;
        bool in_lru = false;
        std::list<std::size_t>::iterator lru_position{};
    };

    void unpin(PageId id, bool dirty);
    std::size_t acquire_frame();
    void detach_from_lru(Frame& frame);
    void read_page(PageId id, std::uint8_t* into);
    void write_page(PageId id, const std::uint8_t* from);
    void write_file_page(PageId id, const std::uint8_t* from);
    void purge_pool();
    void open_file();
    void load_header();
    void store_header();

    std::string path_;
    bool memory_only_ = false;
    std::fstream file_;
    std::unique_ptr<Wal> wal_;
    // Cached so a read does not have to seek to EOF just to bounds-check.
    std::streamoff file_size_ = 0;

    // unique_ptr so that growing the pool never moves a frame, and therefore
    // never invalidates a pointer an outstanding PageGuard is holding.
    std::vector<std::unique_ptr<Frame>> frames_;
    std::vector<std::size_t> free_frames_;
    std::unordered_map<PageId, std::size_t> resident_;
    std::list<std::size_t> lru_;  // front = least recently used

    std::vector<std::uint8_t> header_ = std::vector<std::uint8_t>(kPageSize, 0);
    bool header_dirty_ = false;

    PageId page_count_ = 1;  // page 0 is the header itself
    PageId catalog_root_ = kInvalidPage;

    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::size_t reads_ = 0;
    std::size_t writes_ = 0;
    std::size_t evictions_ = 0;
    std::size_t checkpoints_ = 0;

    // State to restore on rollback. Pages allocated inside an aborted
    // transaction stay allocated in the file; they are simply unreferenced.
    PageId snapshot_page_count_ = 1;
    PageId snapshot_catalog_root_ = kInvalidPage;

    std::size_t checkpoint_threshold_ = 1000;  // frames
};

}  // namespace minidb
