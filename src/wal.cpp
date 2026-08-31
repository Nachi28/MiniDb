#include "minidb/wal.hpp"

#include <cstring>
#include <utility>

#include "minidb/diagnostic.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace minidb {
namespace {

constexpr std::size_t kMagicSize = 8;
const char kMagic[kMagicSize + 1] = "MDBWAL\0";
constexpr std::uint32_t kWalVersion = 1;

constexpr std::size_t kHdrVersion = 8;
constexpr std::size_t kHdrPageSize = 12;
constexpr std::size_t kHdrSalt = 16;
constexpr std::size_t kHdrChecksum = 20;

constexpr std::size_t kFrPage = 0;
constexpr std::size_t kFrDbSize = 4;
constexpr std::size_t kFrSalt = 8;
constexpr std::size_t kFrChecksum = 12;

// FNV-1a. Not cryptographic, and it does not need to be: it exists to notice a
// frame that was half written when the power went out.
std::uint32_t fnv1a(const std::uint8_t* data, std::size_t size, std::uint32_t seed) {
    std::uint32_t hash = seed;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

std::uint32_t frame_checksum(const std::uint8_t* header, const std::uint8_t* image) {
    const std::uint32_t seed = fnv1a(header, kFrChecksum, 2166136261u);
    return fnv1a(image, kPageSize, seed);
}

// MSVC deprecates fopen in favour of fopen_s; everything else has only fopen.
std::FILE* open_stream(const char* path, const char* mode) {
#ifdef _MSC_VER
    std::FILE* file = nullptr;
    if (fopen_s(&file, path, mode) != 0) return nullptr;
    return file;
#else
    return std::fopen(path, mode);
#endif
}

void sync_handle(std::FILE* file) {
    if (file == nullptr) return;
    std::fflush(file);
#ifdef _WIN32
    _commit(_fileno(file));
#else
    fsync(fileno(file));
#endif
}

bool truncate_handle(std::FILE* file, std::int64_t size) {
    if (file == nullptr) return false;
    std::fflush(file);
#ifdef _WIN32
    return _chsize_s(_fileno(file), size) == 0;
#else
    return ftruncate(fileno(file), static_cast<off_t>(size)) == 0;
#endif
}

}  // namespace

Wal::Wal(std::string path, bool memory_only)
    : path_(std::move(path)), memory_only_(memory_only) {
    if (memory_only_) return;
    open_file();
    if (!read_header()) {
        salt_ = 1;
        write_header();
        truncate_to(static_cast<std::int64_t>(kHeaderSize));
    } else {
        recover();
    }
}

Wal::~Wal() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void Wal::open_file() {
    file_ = open_stream(path_.c_str(), "r+b");
    if (file_ == nullptr) {
        file_ = open_stream(path_.c_str(), "w+b");
    }
    if (file_ == nullptr) throw DbError("cannot open write-ahead log: " + path_);
}

void Wal::write_header() {
    std::vector<std::uint8_t> header(kHeaderSize, 0);
    std::memcpy(header.data(), kMagic, kMagicSize);
    put_u32(header.data() + kHdrVersion, kWalVersion);
    put_u32(header.data() + kHdrPageSize, static_cast<std::uint32_t>(kPageSize));
    put_u32(header.data() + kHdrSalt, salt_);
    put_u32(header.data() + kHdrChecksum, fnv1a(header.data(), kHdrChecksum, 2166136261u));

    std::fseek(file_, 0, SEEK_SET);
    if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
        throw DbError("cannot write write-ahead log header: " + path_);
    }
    std::fflush(file_);
}

bool Wal::read_header() {
    std::fseek(file_, 0, SEEK_END);
    const std::int64_t size = std::ftell(file_);
    if (size < static_cast<std::int64_t>(kHeaderSize)) return false;

    std::vector<std::uint8_t> header(kHeaderSize, 0);
    std::fseek(file_, 0, SEEK_SET);
    if (std::fread(header.data(), 1, header.size(), file_) != header.size()) return false;

    if (std::memcmp(header.data(), kMagic, kMagicSize) != 0) return false;
    if (get_u32(header.data() + kHdrVersion) != kWalVersion) return false;
    if (get_u32(header.data() + kHdrPageSize) != kPageSize) return false;
    if (get_u32(header.data() + kHdrChecksum) !=
        fnv1a(header.data(), kHdrChecksum, 2166136261u)) {
        return false;
    }
    salt_ = get_u32(header.data() + kHdrSalt);
    return true;
}

bool Wal::read_frame_at(std::int64_t offset, PageId* page, PageId* db_size,
                        std::uint8_t* image) const {
    std::vector<std::uint8_t> header(kFrameHeaderSize, 0);
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) return false;
    if (std::fread(header.data(), 1, header.size(), file_) != header.size()) return false;
    if (std::fread(image, 1, kPageSize, file_) != kPageSize) return false;

    if (get_u32(header.data() + kFrSalt) != salt_) return false;  // a stale generation
    if (get_u32(header.data() + kFrChecksum) != frame_checksum(header.data(), image)) {
        return false;  // torn or corrupt
    }

    *page = get_u32(header.data() + kFrPage);
    *db_size = get_u32(header.data() + kFrDbSize);
    return true;
}

void Wal::recover() {
    std::fseek(file_, 0, SEEK_END);
    const std::int64_t size = std::ftell(file_);

    std::vector<std::uint8_t> image(kPageSize, 0);
    std::int64_t offset = static_cast<std::int64_t>(kHeaderSize);
    end_ = offset;
    committed_end_ = offset;
    frames_ = 0;

    while (offset + static_cast<std::int64_t>(kFrameSize) <= size) {
        PageId page = 0;
        PageId db_size = 0;
        if (!read_frame_at(offset, &page, &db_size, image.data())) break;

        live_[page] = offset;
        offset += static_cast<std::int64_t>(kFrameSize);
        ++frames_;

        if (db_size != 0) {  // a commit marker: everything so far is durable
            committed_ = live_;
            committed_end_ = offset;
            committed_db_size_ = db_size;
        }
    }

    // Whatever followed the last commit was never promised to anyone, and may
    // be a half-written frame. Drop it.
    rollback_to_commit();
}

bool Wal::contains(PageId page) const {
    if (memory_only_) return false;
    return live_.find(page) != live_.end();
}

bool Wal::read(PageId page, std::uint8_t* into) const {
    if (memory_only_) return false;
    const auto it = live_.find(page);
    if (it == live_.end()) return false;

    if (std::fseek(file_, static_cast<long>(it->second + static_cast<std::int64_t>(kFrameHeaderSize)),
                   SEEK_SET) != 0) {
        throw DbError("cannot seek in write-ahead log: " + path_);
    }
    if (std::fread(into, 1, kPageSize, file_) != kPageSize) {
        throw DbError("short read from write-ahead log: " + path_);
    }
    return true;
}

void Wal::append(PageId page, const std::uint8_t* image, PageId db_size) {
    if (memory_only_) return;

    std::vector<std::uint8_t> header(kFrameHeaderSize, 0);
    put_u32(header.data() + kFrPage, page);
    put_u32(header.data() + kFrDbSize, db_size);
    put_u32(header.data() + kFrSalt, salt_);
    put_u32(header.data() + kFrChecksum, frame_checksum(header.data(), image));

    if (std::fseek(file_, static_cast<long>(end_), SEEK_SET) != 0) {
        throw DbError("cannot seek in write-ahead log: " + path_);
    }
    if (std::fwrite(header.data(), 1, header.size(), file_) != header.size() ||
        std::fwrite(image, 1, kPageSize, file_) != kPageSize) {
        throw DbError("cannot append to write-ahead log: " + path_);
    }

    live_[page] = end_;
    end_ += static_cast<std::int64_t>(kFrameSize);
    ++frames_;

    if (db_size != 0) {
        committed_ = live_;
        committed_end_ = end_;
        committed_db_size_ = db_size;
    }
}

void Wal::sync() {
    if (memory_only_) return;
    sync_handle(file_);
}

void Wal::truncate_to(std::int64_t size) {
    if (memory_only_ || file_ == nullptr) return;
    truncate_handle(file_, size);
}

void Wal::rollback_to_commit() {
    if (memory_only_) return;
    live_ = committed_;
    end_ = committed_end_;
    frames_ = static_cast<std::size_t>((end_ - static_cast<std::int64_t>(kHeaderSize)) /
                                       static_cast<std::int64_t>(kFrameSize));
    truncate_to(end_);
}

void Wal::for_each_committed(const PageVisitor& visit) const {
    if (memory_only_) return;
    std::vector<std::uint8_t> image(kPageSize, 0);
    for (const auto& entry : committed_) {
        PageId page = 0;
        PageId db_size = 0;
        if (!read_frame_at(entry.second, &page, &db_size, image.data())) {
            throw DbError("write-ahead log frame failed verification during checkpoint");
        }
        visit(page, image.data());
    }
}

void Wal::reset() {
    if (memory_only_) return;
    ++salt_;  // any frame from the previous generation is now unrecognisable
    if (salt_ == 0) salt_ = 1;

    live_.clear();
    committed_.clear();
    end_ = static_cast<std::int64_t>(kHeaderSize);
    committed_end_ = end_;
    frames_ = 0;
    committed_db_size_ = 0;

    write_header();
    truncate_to(end_);
    sync_handle(file_);
}

}  // namespace minidb
