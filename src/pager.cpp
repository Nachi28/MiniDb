#include "minidb/pager.hpp"

#include <algorithm>
#include <utility>

#include "minidb/diagnostic.hpp"

namespace minidb {
namespace {

// File header layout, on page 0.
constexpr std::size_t kMagicSize = 6;
const char kMagic[kMagicSize + 1] = "MINIDB";
constexpr std::size_t kOffVersion = 6;
constexpr std::size_t kOffPageCount = 8;
constexpr std::size_t kOffCatalogRoot = 12;
constexpr std::uint8_t kFormatVersion = 1;

}  // namespace

// ---------------------------------------------------------------- PageGuard

PageGuard::PageGuard(Pager* pager, PageId id, std::uint8_t* data)
    : pager_(pager), id_(id), data_(data) {}

PageGuard::~PageGuard() { release(); }

PageGuard::PageGuard(PageGuard&& other) noexcept
    : pager_(other.pager_), id_(other.id_), data_(other.data_), dirty_(other.dirty_) {
    other.pager_ = nullptr;
    other.data_ = nullptr;
    other.dirty_ = false;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
    if (this != &other) {
        release();
        pager_ = other.pager_;
        id_ = other.id_;
        data_ = other.data_;
        dirty_ = other.dirty_;
        other.pager_ = nullptr;
        other.data_ = nullptr;
        other.dirty_ = false;
    }
    return *this;
}

std::uint8_t* PageGuard::mutable_data() {
    dirty_ = true;
    return data_;
}

void PageGuard::release() {
    if (pager_ != nullptr && data_ != nullptr) {
        pager_->unpin(id_, dirty_);
    }
    pager_ = nullptr;
    data_ = nullptr;
    dirty_ = false;
}

// -------------------------------------------------------------------- Pager

Pager::Pager(std::string path, std::size_t pool_pages) : path_(std::move(path)) {
    memory_only_ = (path_ == kMemoryPath);

    const std::size_t initial = memory_only_ ? 16 : (pool_pages < 4 ? 4 : pool_pages);
    frames_.reserve(initial);
    for (std::size_t i = 0; i < initial; ++i) {
        frames_.push_back(std::make_unique<Frame>());
        free_frames_.push_back(i);
    }

    if (!memory_only_) {
        open_file();
        // Opening the log recovers it: frames after the last commit marker,
        // including a half-written one, are discarded. Folding what survives
        // into the database file makes the file authoritative again before
        // anything reads the header.
        wal_ = std::make_unique<Wal>(path_ + "-wal", false);
        if (!wal_->empty()) checkpoint();
        load_header();
    } else {
        wal_ = std::make_unique<Wal>(std::string(), true);
        store_header();  // keep the in-memory header self-consistent
    }
    snapshot_page_count_ = page_count_;
    snapshot_catalog_root_ = catalog_root_;
}

Pager::~Pager() {
    try {
        commit();
        checkpoint();  // leave the database file standing on its own
    } catch (...) {
        // A destructor must not throw; a failed final flush is reported by the
        // explicit flush() that well-behaved callers make before closing.
    }
}

void Pager::open_file() {
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        std::ofstream create(path_, std::ios::binary);
        if (!create) throw DbError("cannot create database file: " + path_);
        create.close();
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    }
    if (!file_.is_open()) throw DbError("cannot open database file: " + path_);

    file_.seekg(0, std::ios::end);
    file_size_ = file_.tellg();
    file_.clear();
}

void Pager::load_header() {
    file_.seekg(0, std::ios::end);
    const std::streamoff size = file_.tellg();
    file_.clear();

    if (size < static_cast<std::streamoff>(kPageSize)) {  // brand new file
        store_header();
        return;
    }

    file_.seekg(0, std::ios::beg);
    file_.read(reinterpret_cast<char*>(header_.data()), static_cast<std::streamsize>(kPageSize));
    file_.clear();

    if (std::memcmp(header_.data(), kMagic, kMagicSize) != 0) {
        throw DbError("not a MiniDB database file: " + path_);
    }
    if (header_[kOffVersion] != kFormatVersion) {
        throw DbError("unsupported MiniDB file format version " +
                      std::to_string(static_cast<int>(header_[kOffVersion])));
    }
    page_count_ = get_u32(header_.data() + kOffPageCount);
    catalog_root_ = get_u32(header_.data() + kOffCatalogRoot);
    if (page_count_ < 1) page_count_ = 1;
}

void Pager::store_header() {
    std::memcpy(header_.data(), kMagic, kMagicSize);
    header_[kOffVersion] = kFormatVersion;
    put_u32(header_.data() + kOffPageCount, page_count_);
    put_u32(header_.data() + kOffCatalogRoot, catalog_root_);
    header_dirty_ = true;
}

void Pager::set_catalog_root(PageId id) {
    catalog_root_ = id;
    store_header();
}

void Pager::read_page(PageId id, std::uint8_t* into) {
    std::fill(into, into + kPageSize, static_cast<std::uint8_t>(0));
    if (memory_only_) return;

    // The log holds the newest image of any page written since the last
    // checkpoint, so it wins over the database file.
    if (wal_ && wal_->read(id, into)) {
        ++reads_;
        return;
    }

    const std::streamoff offset = static_cast<std::streamoff>(id) *
                                  static_cast<std::streamoff>(kPageSize);
    if (offset >= file_size_) return;  // allocated but never written: all zeroes

    file_.seekg(offset, std::ios::beg);
    file_.read(reinterpret_cast<char*>(into), static_cast<std::streamsize>(kPageSize));
    file_.clear();  // a short read at EOF leaves the tail zeroed, which is correct
    ++reads_;
}

// A dirty page leaving the pool mid-transaction is appended to the log as a
// non-commit frame. It is visible to this connection immediately and becomes
// durable only when a commit marker follows it.
void Pager::write_page(PageId id, const std::uint8_t* from) {
    if (memory_only_) return;
    wal_->append(id, from, 0);
    ++writes_;
}

void Pager::write_file_page(PageId id, const std::uint8_t* from) {
    if (memory_only_) return;
    const std::streamoff offset = static_cast<std::streamoff>(id) *
                                  static_cast<std::streamoff>(kPageSize);
    file_.seekp(offset, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(from), static_cast<std::streamsize>(kPageSize));
    if (!file_) throw DbError("write failed on database file: " + path_);

    const std::streamoff end = offset + static_cast<std::streamoff>(kPageSize);
    if (end > file_size_) file_size_ = end;
}

void Pager::detach_from_lru(Frame& frame) {
    if (frame.in_lru) {
        lru_.erase(frame.lru_position);
        frame.in_lru = false;
    }
}

std::size_t Pager::acquire_frame() {
    if (!free_frames_.empty()) {
        const std::size_t index = free_frames_.back();
        free_frames_.pop_back();
        return index;
    }

    // An in-memory database never evicts; it grows instead. Frames are held by
    // unique_ptr, so growing the vector cannot move the buffers out from under
    // an outstanding PageGuard.
    if (memory_only_) {
        frames_.push_back(std::make_unique<Frame>());
        return frames_.size() - 1;
    }

    if (lru_.empty()) {
        throw DbError("buffer pool exhausted: every page is pinned");
    }

    const std::size_t victim = lru_.front();
    Frame& frame = *frames_[victim];
    detach_from_lru(frame);

    if (frame.dirty) write_page(frame.id, frame.data.data());
    resident_.erase(frame.id);
    ++evictions_;

    frame.id = kInvalidPage;
    frame.dirty = false;
    frame.pins = 0;
    return victim;
}

PageGuard Pager::fetch(PageId id) {
    if (id == kInvalidPage || id >= page_count_) {
        throw DbError("page " + std::to_string(id) + " is out of range");
    }

    const auto it = resident_.find(id);
    if (it != resident_.end()) {
        Frame& frame = *frames_[it->second];
        detach_from_lru(frame);
        ++frame.pins;
        ++hits_;
        return PageGuard(this, id, frame.data.data());
    }

    ++misses_;
    const std::size_t index = acquire_frame();
    Frame& frame = *frames_[index];
    read_page(id, frame.data.data());
    frame.id = id;
    frame.dirty = false;
    frame.pins = 1;
    resident_[id] = index;
    return PageGuard(this, id, frame.data.data());
}

PageGuard Pager::allocate(PageId* out_id) {
    const PageId id = page_count_;
    ++page_count_;
    store_header();

    const std::size_t index = acquire_frame();
    Frame& frame = *frames_[index];
    std::fill(frame.data.begin(), frame.data.end(), static_cast<std::uint8_t>(0));
    frame.id = id;
    frame.dirty = true;  // it must reach disk even if nobody writes to it
    frame.pins = 1;
    resident_[id] = index;

    if (out_id != nullptr) *out_id = id;
    return PageGuard(this, id, frame.data.data());
}

void Pager::unpin(PageId id, bool dirty) {
    const auto it = resident_.find(id);
    if (it == resident_.end()) return;

    Frame& frame = *frames_[it->second];
    if (dirty) frame.dirty = true;
    if (frame.pins > 0) --frame.pins;

    if (frame.pins == 0 && !frame.in_lru) {
        lru_.push_back(it->second);
        frame.lru_position = std::prev(lru_.end());
        frame.in_lru = true;
    }
}

void Pager::commit() {
    if (memory_only_) {
        header_dirty_ = false;
        return;
    }
    if (!file_.is_open() || !wal_) return;

    store_header();  // page_count_ and catalog_root_ may have moved

    for (const std::unique_ptr<Frame>& frame : frames_) {
        if (frame->id != kInvalidPage && frame->dirty) {
            wal_->append(frame->id, frame->data.data(), 0);
            frame->dirty = false;
            ++writes_;
        }
    }

    // The header goes last and carries the database size, which is what makes
    // it the commit marker. Recovery replays up to here and no further.
    wal_->append(0, header_.data(), page_count_);
    header_dirty_ = false;
    wal_->sync();

    snapshot_page_count_ = page_count_;
    snapshot_catalog_root_ = catalog_root_;

    if (wal_->frame_count() >= checkpoint_threshold_) checkpoint();
}

void Pager::purge_pool() {
    for (const std::unique_ptr<Frame>& frame : frames_) {
        if (frame->pins > 0) {
            throw DbError("cannot roll back while a page is still pinned");
        }
    }
    resident_.clear();
    lru_.clear();
    free_frames_.clear();
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        frames_[i]->id = kInvalidPage;
        frames_[i]->dirty = false;
        frames_[i]->pins = 0;
        frames_[i]->in_lru = false;
        free_frames_.push_back(i);
    }
}

void Pager::rollback() {
    if (memory_only_ || !wal_) return;

    // Nothing dirty in the pool may survive, because some of it was already
    // spilled into the log and the rest contradicts the durable state.
    purge_pool();
    wal_->rollback_to_commit();

    page_count_ = snapshot_page_count_;
    catalog_root_ = snapshot_catalog_root_;
    store_header();
    header_dirty_ = false;
}

void Pager::checkpoint() {
    if (memory_only_ || !wal_ || !file_.is_open()) return;
    if (wal_->committed_pages() == 0) {
        wal_->reset();
        return;
    }

    wal_->for_each_committed([&](PageId page, const std::uint8_t* image) {
        write_file_page(page, image);
        ++writes_;
    });
    file_.flush();

    // Only once the database file holds everything is it safe to forget the
    // log. Resetting it first would lose the pages on a crash in between.
    wal_->reset();
    ++checkpoints_;

    purge_pool();  // cached images may predate what was just written
}
}  // namespace minidb
