#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "minidb/page.hpp"

namespace minidb {

// A write-ahead log of whole page images.
//
// Changed pages are appended here instead of being written in place. A frame
// carrying a non-zero database size is a commit marker: everything up to and
// including it is durable, and anything after it is not. Recovery therefore
// needs no undo information -- it replays forward to the last commit and throws
// away the tail, which is exactly what a torn write leaves behind.
//
// File layout, little-endian throughout:
//
//   header  32 bytes   magic, format version, page size, salt, checksum
//   frame   24 bytes   page number, db size (0 unless commit), salt, checksum
//           + kPageSize bytes of page image
//
// The salt changes every time the log is reset, so stale frames left over from
// a previous generation can never be mistaken for current ones.
//
// An in-memory database has nothing to be durable against, so every method
// here is a no-op in that mode.
class Wal {
public:
    static constexpr std::size_t kHeaderSize = 32;
    static constexpr std::size_t kFrameHeaderSize = 24;
    static constexpr std::size_t kFrameSize = kFrameHeaderSize + kPageSize;

    Wal(std::string path, bool memory_only);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // Reads the newest visible image of `page`. False if the log has none.
    bool read(PageId page, std::uint8_t* into) const;
    bool contains(PageId page) const;

    // Appends a page image. `db_size` non-zero makes this a commit marker.
    void append(PageId page, const std::uint8_t* image, PageId db_size);

    void sync();

    // Drops every frame written since the last commit marker, which is what a
    // ROLLBACK needs and what recovery does to a torn tail.
    void rollback_to_commit();

    // Hands each page committed in the log to the caller, newest image only,
    // so the pager can fold them into the database file.
    using PageVisitor = std::function<void(PageId, const std::uint8_t*)>;
    void for_each_committed(const PageVisitor& visit) const;

    // Empties the log and rolls the salt. Call after a successful checkpoint.
    void reset();

    std::size_t frame_count() const { return frames_; }
    std::size_t committed_pages() const { return committed_.size(); }
    PageId committed_db_size() const { return committed_db_size_; }
    bool empty() const { return committed_.empty() && live_.empty(); }
    const std::string& path() const { return path_; }

private:
    void open_file();
    void write_header();
    bool read_header();
    void recover();
    bool read_frame_at(std::int64_t offset, PageId* page, PageId* db_size,
                       std::uint8_t* image) const;
    void truncate_to(std::int64_t size);

    std::string path_;
    bool memory_only_ = false;
    std::FILE* file_ = nullptr;

    std::uint32_t salt_ = 1;
    std::int64_t end_ = static_cast<std::int64_t>(kHeaderSize);
    std::int64_t committed_end_ = static_cast<std::int64_t>(kHeaderSize);
    std::size_t frames_ = 0;
    PageId committed_db_size_ = 0;

    // Newest frame offset per page: `live_` includes frames not yet committed,
    // `committed_` is the snapshot a rollback returns to.
    std::unordered_map<PageId, std::int64_t> live_;
    std::unordered_map<PageId, std::int64_t> committed_;

};

}  // namespace minidb
