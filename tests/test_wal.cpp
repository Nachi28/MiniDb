#include "minidb/wal.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "minidb/diagnostic.hpp"
#include "test_framework.hpp"

using namespace minidb;

namespace {

struct TempWal {
    std::string path;
    explicit TempWal(const std::string& name) : path("minidb_wal_" + name + ".log") {
        std::remove(path.c_str());
    }
    ~TempWal() { std::remove(path.c_str()); }
    TempWal(const TempWal&) = delete;
    TempWal& operator=(const TempWal&) = delete;
};

std::FILE* open_raw(const std::string& path, const char* mode) {
#ifdef _MSC_VER
    std::FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), mode) != 0) return nullptr;
    return f;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::vector<std::uint8_t> image_of(std::uint8_t fill, PageId page) {
    std::vector<std::uint8_t> image(kPageSize, fill);
    put_u32(image.data(), page);
    return image;
}

bool image_is(const Wal& wal, PageId page, std::uint8_t fill) {
    std::vector<std::uint8_t> got(kPageSize, 0);
    if (!wal.read(page, got.data())) return false;
    return got == image_of(fill, page);
}

std::int64_t file_size(const std::string& path) {
    std::FILE* f = open_raw(path, "rb");
    if (f == nullptr) return -1;
    std::fseek(f, 0, SEEK_END);
    const std::int64_t size = std::ftell(f);
    std::fclose(f);
    return size;
}

// Chops the file at `size`, which is what an interrupted write leaves behind.
void truncate_file(const std::string& path, std::int64_t size) {
    const std::int64_t original = file_size(path);
    std::vector<char> bytes(static_cast<std::size_t>(original < 0 ? 0 : original), 0);
    std::FILE* in = open_raw(path, "rb");
    REQUIRE(in != nullptr);
    std::fread(bytes.data(), 1, bytes.size(), in);
    std::fclose(in);

    std::FILE* out = open_raw(path, "wb");
    REQUIRE(out != nullptr);
    std::fwrite(bytes.data(), 1, static_cast<std::size_t>(size), out);
    std::fclose(out);
}

void poke(const std::string& path, std::int64_t offset, int value) {
    std::FILE* f = open_raw(path, "r+b");
    REQUIRE(f != nullptr);
    std::fseek(f, static_cast<long>(offset), SEEK_SET);
    std::fputc(value, f);
    std::fclose(f);
}

}  // namespace

TEST(Wal, StartsEmpty) {
    TempWal file("empty");
    Wal wal(file.path, false);
    REQUIRE(wal.empty());
    REQUIRE_EQ(wal.frame_count(), static_cast<std::size_t>(0));
    std::vector<std::uint8_t> scratch(kPageSize, 0);
    REQUIRE(!wal.read(1, scratch.data()));
}

TEST(Wal, ReadsBackTheNewestImageOfAPage) {
    TempWal file("newest");
    Wal wal(file.path, false);
    wal.append(5, image_of(0x11, 5).data(), 0);
    REQUIRE(image_is(wal, 5, 0x11));
    wal.append(5, image_of(0x22, 5).data(), 0);
    REQUIRE(image_is(wal, 5, 0x22));
    REQUIRE_EQ(wal.frame_count(), static_cast<std::size_t>(2));
}

TEST(Wal, RollbackDropsEverythingAfterTheLastCommit) {
    TempWal file("rollback");
    Wal wal(file.path, false);
    wal.append(1, image_of(0xAA, 1).data(), 0);
    wal.append(0, image_of(0x01, 0).data(), 4);  // commit marker
    wal.append(1, image_of(0xBB, 1).data(), 0);
    REQUIRE(image_is(wal, 1, 0xBB));

    wal.rollback_to_commit();
    REQUIRE(image_is(wal, 1, 0xAA));
    REQUIRE_EQ(wal.committed_db_size(), static_cast<PageId>(4));
}

TEST(Wal, RecoveryKeepsCommittedFramesAndDiscardsTheTail) {
    TempWal file("recover");
    {
        Wal wal(file.path, false);
        wal.append(1, image_of(0xAA, 1).data(), 0);
        wal.append(2, image_of(0xBB, 2).data(), 0);
        wal.append(0, image_of(0x01, 0).data(), 3);  // commit
        wal.append(1, image_of(0xCC, 1).data(), 0);  // never committed
        wal.append(2, image_of(0xDD, 2).data(), 0);
        wal.sync();
    }
    {
        Wal wal(file.path, false);
        REQUIRE(image_is(wal, 1, 0xAA));
        REQUIRE(image_is(wal, 2, 0xBB));
        REQUIRE_EQ(wal.committed_db_size(), static_cast<PageId>(3));
    }
}

TEST(Wal, AHalfWrittenTrailingFrameIsIgnored) {
    TempWal file("torn");
    {
        Wal wal(file.path, false);
        wal.append(1, image_of(0xAA, 1).data(), 0);
        wal.append(0, image_of(0x01, 0).data(), 2);  // commit
        wal.append(1, image_of(0xCC, 1).data(), 0);
        wal.sync();
    }
    // Cut the last frame in half, exactly as an interrupted write would.
    truncate_file(file.path,
                  file_size(file.path) - static_cast<std::int64_t>(kPageSize / 2));
    {
        Wal wal(file.path, false);
        REQUIRE(image_is(wal, 1, 0xAA));  // the committed value, not the torn one
    }
}

TEST(Wal, ACorruptedFrameStopsRecoveryAtThatPoint) {
    TempWal file("corrupt");
    {
        Wal wal(file.path, false);
        wal.append(1, image_of(0xAA, 1).data(), 0);
        wal.append(0, image_of(0x01, 0).data(), 2);  // commit after frame 2
        wal.append(3, image_of(0xEE, 3).data(), 0);
        wal.append(0, image_of(0x02, 0).data(), 4);  // commit after frame 4
        wal.sync();
    }
    // Flip a byte inside the third frame's image. Recovery must distrust it, and
    // therefore must not trust the commit that comes after it either.
    const std::int64_t offset = static_cast<std::int64_t>(Wal::kHeaderSize) +
                                2 * static_cast<std::int64_t>(Wal::kFrameSize) +
                                static_cast<std::int64_t>(Wal::kFrameHeaderSize) + 40;
    poke(file.path, offset, 0x7E);
    {
        Wal wal(file.path, false);
        REQUIRE(image_is(wal, 1, 0xAA));
        REQUIRE_EQ(wal.committed_db_size(), static_cast<PageId>(2));
        std::vector<std::uint8_t> scratch(kPageSize, 0);
        REQUIRE(!wal.read(3, scratch.data()));
    }
}

TEST(Wal, ResetRollsTheSaltSoOldFramesAreUnreadable) {
    TempWal file("salt");
    {
        Wal wal(file.path, false);
        wal.append(1, image_of(0xAA, 1).data(), 0);
        wal.append(0, image_of(0x01, 0).data(), 2);
        wal.sync();
        wal.reset();
        REQUIRE(wal.empty());
        REQUIRE_EQ(wal.frame_count(), static_cast<std::size_t>(0));
    }
    {
        Wal wal(file.path, false);
        REQUIRE(wal.empty());  // the old generation is gone, not merely hidden
    }
}

TEST(Wal, CommittedPagesAreVisitedOnceWithTheirNewestImage) {
    TempWal file("visit");
    Wal wal(file.path, false);
    wal.append(1, image_of(0x11, 1).data(), 0);
    wal.append(1, image_of(0x22, 1).data(), 0);  // supersedes the first
    wal.append(2, image_of(0x33, 2).data(), 0);
    wal.append(0, image_of(0x01, 0).data(), 3);

    int visits = 0;
    bool saw_newest = false;
    wal.for_each_committed([&](PageId page, const std::uint8_t* image) {
        ++visits;
        if (page == 1 && image[10] == 0x22) saw_newest = true;
    });
    REQUIRE_EQ(visits, 3);  // pages 0, 1 and 2
    REQUIRE(saw_newest);
}

TEST(Wal, InMemoryModeIsANoOp) {
    Wal wal(std::string(), true);
    std::vector<std::uint8_t> scratch(kPageSize, 0);
    wal.append(1, scratch.data(), 1);
    REQUIRE(wal.empty());
    REQUIRE(!wal.read(1, scratch.data()));
    REQUIRE_EQ(wal.frame_count(), static_cast<std::size_t>(0));
}
