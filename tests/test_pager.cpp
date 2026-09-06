#include "minidb/pager.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "minidb/diagnostic.hpp"
#include "test_framework.hpp"

using namespace minidb;

namespace {

// A database file in the working directory, removed on the way in and out so a
// crashed run cannot poison the next one.
struct TempDb {
    std::string path;
    explicit TempDb(const std::string& name) : path("minidb_test_" + name + ".db") {
        std::remove(path.c_str());
    }
    ~TempDb() { std::remove(path.c_str()); }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;
};

void stamp(Pager& pager, PageId id, std::uint8_t byte) {
    PageGuard guard = pager.fetch(id);
    std::uint8_t* data = guard.mutable_data();
    data[0] = byte;
    put_u32(data + 8, id);
}

bool has_stamp(Pager& pager, PageId id, std::uint8_t byte) {
    PageGuard guard = pager.fetch(id);
    return guard.data()[0] == byte && get_u32(guard.data() + 8) == id;
}

}  // namespace

TEST(Pager, StartsWithOnlyTheHeaderPage) {
    Pager pager(Pager::kMemoryPath);
    REQUIRE_EQ(pager.page_count(), static_cast<PageId>(1));
    REQUIRE(pager.memory_only());
}

TEST(Pager, AllocatesConsecutivePagesStartingAtOne) {
    Pager pager(Pager::kMemoryPath);
    PageId first = kInvalidPage;
    PageId second = kInvalidPage;
    PageGuard a = pager.allocate(&first);
    PageGuard b = pager.allocate(&second);
    REQUIRE_EQ(first, static_cast<PageId>(1));
    REQUIRE_EQ(second, static_cast<PageId>(2));
    REQUIRE_EQ(pager.page_count(), static_cast<PageId>(3));
    REQUIRE(a.valid());
    REQUIRE(b.valid());
}

TEST(Pager, NewPagesAreZeroed) {
    Pager pager(Pager::kMemoryPath);
    PageId id = kInvalidPage;
    PageGuard guard = pager.allocate(&id);
    for (std::size_t i = 0; i < kPageSize; ++i) {
        REQUIRE_EQ(guard.data()[i], static_cast<std::uint8_t>(0));
    }
}

TEST(Pager, WritesAreVisibleOnRefetch) {
    Pager pager(Pager::kMemoryPath);
    PageId id = kInvalidPage;
    pager.allocate(&id);
    stamp(pager, id, 0xAB);
    REQUIRE(has_stamp(pager, id, 0xAB));
}

TEST(Pager, RejectsTheHeaderPageAndOutOfRangePages) {
    Pager pager(Pager::kMemoryPath);
    REQUIRE_THROWS_AS(pager.fetch(kInvalidPage), DbError);
    REQUIRE_THROWS_AS(pager.fetch(99), DbError);
}

TEST(Pager, CountsHitsAndMisses) {
    Pager pager(Pager::kMemoryPath);
    PageId id = kInvalidPage;
    pager.allocate(&id);          // resident, and unpinned when the guard dies
    const std::size_t before = pager.cache_hits();
    { PageGuard g = pager.fetch(id); }
    { PageGuard g = pager.fetch(id); }
    REQUIRE_EQ(pager.cache_hits(), before + 2);
}

TEST(Pager, EvictsUnpinnedPagesAndWritesThemBack) {
    TempDb db("evict");
    {
        Pager pager(db.path, 4);  // pool far smaller than the working set
        std::vector<PageId> ids;
        for (int i = 0; i < 40; ++i) {
            PageId id = kInvalidPage;
            pager.allocate(&id);  // guard dies immediately, so the page is evictable
            ids.push_back(id);
            stamp(pager, id, static_cast<std::uint8_t>(i + 1));
        }
        REQUIRE(pager.evictions() > 0);
        for (int i = 0; i < 40; ++i) {
            REQUIRE(has_stamp(pager, ids[static_cast<std::size_t>(i)],
                              static_cast<std::uint8_t>(i + 1)));
        }
    }
}

TEST(Pager, PinnedPagesAreNeverEvicted) {
    TempDb db("pinned");
    Pager pager(db.path, 4);

    PageId pinned = kInvalidPage;
    PageGuard hold = pager.allocate(&pinned);
    hold.mutable_data()[0] = 0x7F;
    put_u32(hold.mutable_data() + 8, pinned);

    // Churn far more pages than the pool holds. The pinned page must stay put.
    for (int i = 0; i < 20; ++i) {
        PageId id = kInvalidPage;
        pager.allocate(&id);
    }

    REQUIRE_EQ(hold.data()[0], static_cast<std::uint8_t>(0x7F));  // pointer still valid
    hold.release();
    REQUIRE(has_stamp(pager, pinned, 0x7F));  // and it survives once evictable
}

TEST(Pager, ReportsAFullyPinnedPool) {
    TempDb db("exhausted");
    Pager pager(db.path, 4);
    std::vector<PageGuard> held;
    bool threw = false;
    try {
        for (int i = 0; i < 32; ++i) {
            PageId id = kInvalidPage;
            held.push_back(pager.allocate(&id));
        }
    } catch (const DbError&) {
        threw = true;
    }
    REQUIRE(threw);
}

TEST(Pager, DataSurvivesCloseAndReopen) {
    TempDb db("persist");
    std::vector<PageId> ids;
    {
        Pager pager(db.path, 8);
        for (int i = 0; i < 25; ++i) {
            PageId id = kInvalidPage;
            pager.allocate(&id);
            ids.push_back(id);
            stamp(pager, id, static_cast<std::uint8_t>(i + 1));
        }
        pager.flush();
    }
    {
        Pager pager(db.path, 8);
        REQUIRE_EQ(pager.page_count(), static_cast<PageId>(26));
        for (int i = 0; i < 25; ++i) {
            REQUIRE(has_stamp(pager, ids[static_cast<std::size_t>(i)],
                              static_cast<std::uint8_t>(i + 1)));
        }
        REQUIRE(pager.disk_reads() > 0);
    }
}

TEST(Pager, TheDestructorFlushesWithoutAnExplicitCall) {
    TempDb db("dtor");
    PageId id = kInvalidPage;
    {
        Pager pager(db.path, 8);
        pager.allocate(&id);
        stamp(pager, id, 0x5A);
    }
    {
        Pager pager(db.path, 8);
        REQUIRE(has_stamp(pager, id, 0x5A));
    }
}

TEST(Pager, CatalogRootSurvivesReopen) {
    TempDb db("catalog");
    {
        Pager pager(db.path, 8);
        REQUIRE_EQ(pager.catalog_root(), kInvalidPage);
        PageId id = kInvalidPage;
        pager.allocate(&id);
        pager.set_catalog_root(id);
        pager.flush();
    }
    {
        Pager pager(db.path, 8);
        REQUIRE_EQ(pager.catalog_root(), static_cast<PageId>(1));
    }
}

TEST(Pager, RejectsAFileThatIsNotAMiniDbDatabase) {
    TempDb db("foreign");
    {
        std::FILE* f = std::fopen(db.path.c_str(), "wb");
        REQUIRE(f != nullptr);
        std::vector<char> junk(static_cast<std::size_t>(kPageSize), 'x');
        std::fwrite(junk.data(), 1, junk.size(), f);
        std::fclose(f);
    }
    bool threw = false;
    try {
        Pager pager(db.path, 8);
    } catch (const DbError&) {
        threw = true;
    }
    REQUIRE(threw);
}

TEST(Pager, MemoryOnlyDatabasesNeverTouchDisk) {
    Pager pager(Pager::kMemoryPath, 4);
    std::vector<PageId> ids;
    for (int i = 0; i < 50; ++i) {  // far more than the nominal pool size
        PageId id = kInvalidPage;
        pager.allocate(&id);
        ids.push_back(id);
        stamp(pager, id, static_cast<std::uint8_t>(i + 1));
    }
    for (int i = 0; i < 50; ++i) {
        REQUIRE(has_stamp(pager, ids[static_cast<std::size_t>(i)],
                          static_cast<std::uint8_t>(i + 1)));
    }
    REQUIRE_EQ(pager.disk_writes(), static_cast<std::size_t>(0));
    REQUIRE_EQ(pager.disk_reads(), static_cast<std::size_t>(0));
    REQUIRE_EQ(pager.evictions(), static_cast<std::size_t>(0));
}

TEST(Pager, RefusesToOpenAShortForeignFileInsteadOfClobberingIt) {
    // A file smaller than one page used to be treated as a brand new database,
    // so pointing --db at, say, a SQL script silently overwrote it on flush.
    TempDb db("shortfile");
    {
        std::FILE* f = nullptr;
#ifdef _MSC_VER
        fopen_s(&f, db.path.c_str(), "wb");
#else
        f = std::fopen(db.path.c_str(), "wb");
#endif
        REQUIRE(f != nullptr);
        const char* text = "SELECT * FROM players;\n";
        std::fwrite(text, 1, std::strlen(text), f);
        std::fclose(f);
    }

    bool threw = false;
    try {
        Pager pager(db.path, 8);
    } catch (const DbError&) {
        threw = true;
    }
    REQUIRE(threw);

    // ...and the file is still intact.
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    fopen_s(&f, db.path.c_str(), "rb");
#else
    f = std::fopen(db.path.c_str(), "rb");
#endif
    REQUIRE(f != nullptr);
    char buffer[64] = {0};
    const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    REQUIRE(n > 0);
    REQUIRE(std::string(buffer).find("SELECT") == 0);
}

TEST(Pager, StillTreatsATrulyEmptyFileAsANewDatabase) {
    TempDb db("emptyfile");
    {
        std::FILE* f = nullptr;
#ifdef _MSC_VER
        fopen_s(&f, db.path.c_str(), "wb");
#else
        f = std::fopen(db.path.c_str(), "wb");
#endif
        REQUIRE(f != nullptr);
        std::fclose(f);  // zero bytes
    }
    Pager pager(db.path, 8);
    REQUIRE_EQ(pager.page_count(), static_cast<PageId>(1));
}
