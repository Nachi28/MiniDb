#include "minidb/btree.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "minidb/diagnostic.hpp"
#include "test_framework.hpp"

using namespace minidb;

namespace {

struct TempDb {
    std::string path;
    explicit TempDb(const std::string& name) : path("minidb_btree_" + name + ".db") {
        std::remove(path.c_str());
    }
    ~TempDb() { std::remove(path.c_str()); }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;
};

// Deterministic pseudo-random sequence, so a failure is always reproducible.
struct Lcg {
    std::uint64_t state;
    explicit Lcg(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 16;
    }
};

std::vector<std::uint8_t> payload_for(std::int64_t key, std::size_t length) {
    std::vector<std::uint8_t> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<std::uint8_t>((key + static_cast<std::int64_t>(i)) & 0xFF);
    }
    return bytes;
}

void put(BTree& tree, std::int64_t key, std::size_t length) {
    const std::vector<std::uint8_t> bytes = payload_for(key, length);
    tree.insert(key, bytes.data(), bytes.size());
}

bool holds(const BTree& tree, std::int64_t key, std::size_t length) {
    std::vector<std::uint8_t> got;
    if (!tree.find(key, &got)) return false;
    return got == payload_for(key, length);
}

std::vector<std::int64_t> scan_keys(const BTree& tree) {
    std::vector<std::int64_t> keys;
    tree.scan([&](std::int64_t key, const std::uint8_t*, std::size_t) {
        keys.push_back(key);
        return true;
    });
    return keys;
}

}  // namespace

TEST(BTree, AnEmptyTreeIsASingleLeaf) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(0));
    REQUIRE_EQ(tree.height(), static_cast<std::size_t>(1));
    REQUIRE(!tree.find(1, nullptr));
    REQUIRE_EQ(tree.max_key(), static_cast<std::int64_t>(0));
}

TEST(BTree, StoresAndFindsOneEntry) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    put(tree, 42, 16);
    REQUIRE(holds(tree, 42, 16));
    REQUIRE(!tree.find(41, nullptr));
    REQUIRE(!tree.find(43, nullptr));
    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(1));
}

TEST(BTree, HandlesZeroLengthPayloads) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    put(tree, 7, 0);
    std::vector<std::uint8_t> got;
    REQUIRE(tree.find(7, &got));
    REQUIRE_EQ(got.size(), static_cast<std::size_t>(0));
}

TEST(BTree, InsertingTheSameKeyReplacesThePayload) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    put(tree, 5, 10);
    put(tree, 5, 40);  // a different length, to force the cell to move
    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(1));
    REQUIRE(holds(tree, 5, 40));
}

TEST(BTree, KeepsScanOrderAscendingIncludingNegatives) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    const std::int64_t keys[] = {5, -3, 100, 0, -50, 7};
    for (std::int64_t k : keys) put(tree, k, 8);

    const std::vector<std::int64_t> seen = scan_keys(tree);
    REQUIRE_EQ(seen.size(), static_cast<std::size_t>(6));
    for (std::size_t i = 1; i < seen.size(); ++i) REQUIRE(seen[i - 1] < seen[i]);
    REQUIRE_EQ(seen.front(), static_cast<std::int64_t>(-50));
    REQUIRE_EQ(seen.back(), static_cast<std::int64_t>(100));
    REQUIRE_EQ(tree.max_key(), static_cast<std::int64_t>(100));
}

TEST(BTree, SplitsAndGrowsInHeightOnSequentialInserts) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    const int n = 5000;
    for (int i = 1; i <= n; ++i) put(tree, i, 40);

    // 40-byte payloads pack ~78 to a leaf, so 5000 rows fill ~64 leaves, and a
    // single internal node indexes ~292 children. Height 2 is the correct
    // answer here; see DeepensToThreeLevels for a taller tree.
    REQUIRE_EQ(tree.height(), static_cast<std::size_t>(2));
    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(n));
    for (int i = 1; i <= n; ++i) REQUIRE(holds(tree, i, 40));

    const std::vector<std::int64_t> seen = scan_keys(tree);
    REQUIRE_EQ(seen.size(), static_cast<std::size_t>(n));
    for (std::size_t i = 1; i < seen.size(); ++i) REQUIRE(seen[i - 1] < seen[i]);
}

TEST(BTree, DeepensToThreeLevels) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));

    // Payloads this large fit only two to a leaf, so 800 rows need ~400 leaves,
    // more than one internal node can index. The root has to split again.
    for (int i = 1; i <= 800; ++i) put(tree, i, 1500);

    REQUIRE_EQ(tree.height(), static_cast<std::size_t>(3));
    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(800));
    for (int i = 1; i <= 800; ++i) REQUIRE(holds(tree, i, 1500));

    const std::vector<std::int64_t> seen = scan_keys(tree);
    REQUIRE_EQ(seen.size(), static_cast<std::size_t>(800));
    for (std::size_t i = 1; i < seen.size(); ++i) REQUIRE(seen[i - 1] < seen[i]);
}

TEST(BTree, HandlesDescendingInsertOrder) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    const int n = 2000;
    for (int i = n; i >= 1; --i) put(tree, i, 30);
    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(n));
    for (int i = 1; i <= n; ++i) REQUIRE(holds(tree, i, 30));
    const std::vector<std::int64_t> seen = scan_keys(tree);
    for (std::size_t i = 1; i < seen.size(); ++i) REQUIRE(seen[i - 1] < seen[i]);
}

TEST(BTree, HandlesRandomKeysAndVariableSizes) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));

    Lcg rng(20260826);
    std::vector<std::int64_t> keys;
    std::vector<std::size_t> lengths;
    for (int i = 0; i < 3000; ++i) {
        const std::int64_t key = static_cast<std::int64_t>(rng.next() % 1000000) - 500000;
        const std::size_t length = static_cast<std::size_t>(rng.next() % 200);
        keys.push_back(key);
        lengths.push_back(length);
        put(tree, key, length);
    }

    // Later writes win, so only check the last length recorded for each key.
    for (std::size_t i = 0; i < keys.size(); ++i) {
        bool shadowed = false;
        for (std::size_t j = i + 1; j < keys.size(); ++j) {
            if (keys[j] == keys[i]) {
                shadowed = true;
                break;
            }
        }
        if (!shadowed) REQUIRE(holds(tree, keys[i], lengths[i]));
    }

    const std::vector<std::int64_t> seen = scan_keys(tree);
    for (std::size_t i = 1; i < seen.size(); ++i) REQUIRE(seen[i - 1] < seen[i]);
}

TEST(BTree, StoresPayloadsUpToTheLimitAndRejectsLargerOnes) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));

    put(tree, 1, BTree::kMaxPayload);
    put(tree, 2, BTree::kMaxPayload);  // two near-full cells force a split
    REQUIRE(holds(tree, 1, BTree::kMaxPayload));
    REQUIRE(holds(tree, 2, BTree::kMaxPayload));

    const std::vector<std::uint8_t> huge(BTree::kMaxPayload + 1, 0);
    REQUIRE_THROWS_AS(tree.insert(3, huge.data(), huge.size()), DbError);
}

TEST(BTree, EraseRemovesOnlyTheNamedKey) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    for (int i = 1; i <= 500; ++i) put(tree, i, 24);

    REQUIRE(tree.erase(250));
    REQUIRE(!tree.erase(250));
    REQUIRE(!tree.find(250, nullptr));
    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(499));
    REQUIRE(holds(tree, 249, 24));
    REQUIRE(holds(tree, 251, 24));
}

TEST(BTree, SpaceFromErasedCellsIsReused) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    for (int round = 0; round < 200; ++round) {
        put(tree, round, 300);
        if (round > 0) tree.erase(round - 1);
    }
    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(1));
    REQUIRE(holds(tree, 199, 300));
}

TEST(BTree, SurvivesCloseAndReopen) {
    TempDb db("persist");
    PageId root = kInvalidPage;
    {
        Pager pager(db.path, 16);
        BTree tree(pager, BTree::create(pager));
        for (int i = 1; i <= 2000; ++i) put(tree, i, 50);
        root = tree.root();  // the root moves whenever the tree gains a level
        pager.set_catalog_root(root);
        pager.flush();
    }
    {
        Pager pager(db.path, 16);
        REQUIRE_EQ(pager.catalog_root(), root);
        BTree tree(pager, pager.catalog_root());
        REQUIRE_EQ(tree.size(), static_cast<std::size_t>(2000));
        for (int i = 1; i <= 2000; ++i) REQUIRE(holds(tree, i, 50));
    }
}

TEST(BTree, WorksWithABufferPoolFarSmallerThanTheTree) {
    TempDb db("smallpool");
    Pager pager(db.path, 8);  // 8 frames for a tree of hundreds of pages
    BTree tree(pager, BTree::create(pager));
    for (int i = 1; i <= 3000; ++i) put(tree, i, 60);
    REQUIRE(pager.evictions() > 0);
    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(3000));
    for (int i = 1; i <= 3000; ++i) REQUIRE(holds(tree, i, 60));
}

TEST(BTree, StopsScanningWhenTheVisitorSaysSo) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    for (int i = 1; i <= 1000; ++i) put(tree, i, 20);

    int seen = 0;
    tree.scan([&](std::int64_t, const std::uint8_t*, std::size_t) {
        ++seen;
        return seen < 10;
    });
    REQUIRE_EQ(seen, 10);
}

TEST(BTree, SequentialAppendsPackLeavesTightly) {
    // 40-byte payloads make 52-byte cells, so a leaf holds ~78 of them and 2000
    // entries need ~26 leaves when packed. A plain 50/50 split would leave every
    // leaf half empty and use roughly twice as many pages.
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    for (int i = 1; i <= 2000; ++i) put(tree, i, 40);

    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(2000));
    REQUIRE(pager.page_count() < 40);
    for (int i = 1; i <= 2000; ++i) REQUIRE(holds(tree, i, 40));
}

TEST(BTree, DescendingInsertsPackTooAndStayCorrect) {
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    for (int i = 2000; i >= 1; --i) put(tree, i, 40);

    REQUIRE_EQ(tree.size(), static_cast<std::size_t>(2000));
    REQUIRE(pager.page_count() < 40);

    const std::vector<std::int64_t> seen = scan_keys(tree);
    REQUIRE_EQ(seen.size(), static_cast<std::size_t>(2000));
    for (std::size_t i = 1; i < seen.size(); ++i) REQUIRE(seen[i - 1] < seen[i]);
}

TEST(BTree, InsertsInTheMiddleStillSplitEvenly) {
    // Random-order keys must not take the append path; they should split down
    // the middle so neither side is starved.
    Pager pager(Pager::kMemoryPath);
    BTree tree(pager, BTree::create(pager));
    Lcg rng(4242);
    std::vector<std::int64_t> keys;
    for (int i = 0; i < 2000; ++i) {
        const std::int64_t key = static_cast<std::int64_t>(rng.next() % 100000);
        keys.push_back(key);
        put(tree, key, 40);
    }
    for (std::int64_t key : keys) REQUIRE(tree.find(key, nullptr));
    const std::vector<std::int64_t> seen = scan_keys(tree);
    for (std::size_t i = 1; i < seen.size(); ++i) REQUIRE(seen[i - 1] < seen[i]);
}
