#include "minidb/index.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "test_framework.hpp"

using namespace minidb;

namespace {

std::vector<std::int64_t> equal_rowids(const Index& index, const Value& value) {
    std::vector<std::int64_t> rowids;
    index.seek_equal(value, [&](std::int64_t rowid) {
        rowids.push_back(rowid);
        return true;
    });
    return rowids;
}

std::vector<std::int64_t> range_rowids(const Index& index, const Value* low, bool low_inc,
                                       const Value* high, bool high_inc) {
    std::vector<std::int64_t> rowids;
    index.seek_range(low, low_inc, high, high_inc, [&](std::int64_t rowid) {
        rowids.push_back(rowid);
        return true;
    });
    return rowids;
}

}  // namespace

TEST(Index, FindsEveryRowidForAValue) {
    Pager pager(Pager::kMemoryPath);
    Index index("i", "t", "c", pager, BTree::create(pager), 1);

    index.insert(Value::integer(10), 1);
    index.insert(Value::integer(20), 2);
    index.insert(Value::integer(10), 3);
    index.insert(Value::integer(10), 4);

    const std::vector<std::int64_t> found = equal_rowids(index, Value::integer(10));
    REQUIRE_EQ(found.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(found[0], static_cast<std::int64_t>(1));  // ordered by rowid
    REQUIRE_EQ(found[1], static_cast<std::int64_t>(3));
    REQUIRE_EQ(found[2], static_cast<std::int64_t>(4));

    REQUIRE_EQ(equal_rowids(index, Value::integer(20)).size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(equal_rowids(index, Value::integer(99)).size(), static_cast<std::size_t>(0));
}

TEST(Index, IndexesTextAndNull) {
    Pager pager(Pager::kMemoryPath);
    Index index("i", "t", "c", pager, BTree::create(pager), 1);

    index.insert(Value::text("apple"), 1);
    index.insert(Value::null(), 2);
    index.insert(Value::text("banana"), 3);
    index.insert(Value::null(), 4);

    REQUIRE_EQ(equal_rowids(index, Value::text("apple")).size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(equal_rowids(index, Value::null()).size(), static_cast<std::size_t>(2));
}

TEST(Index, RangeBoundsRespectInclusivity) {
    Pager pager(Pager::kMemoryPath);
    Index index("i", "t", "c", pager, BTree::create(pager), 1);
    for (std::int64_t i = 1; i <= 10; ++i) index.insert(Value::integer(i), i);

    const Value low = Value::integer(3);
    const Value high = Value::integer(7);

    REQUIRE_EQ(range_rowids(index, &low, true, &high, true).size(),
               static_cast<std::size_t>(5));  // 3,4,5,6,7
    REQUIRE_EQ(range_rowids(index, &low, false, &high, false).size(),
               static_cast<std::size_t>(3));  // 4,5,6
    REQUIRE_EQ(range_rowids(index, &low, true, &high, false).size(),
               static_cast<std::size_t>(4));  // 3,4,5,6
    REQUIRE_EQ(range_rowids(index, &low, true, nullptr, false).size(),
               static_cast<std::size_t>(8));  // 3..10
    REQUIRE_EQ(range_rowids(index, nullptr, false, &high, true).size(),
               static_cast<std::size_t>(7));  // 1..7
    REQUIRE_EQ(range_rowids(index, nullptr, false, nullptr, false).size(),
               static_cast<std::size_t>(10));
}

TEST(Index, RangeScansAreInValueOrder) {
    Pager pager(Pager::kMemoryPath);
    Index index("i", "t", "c", pager, BTree::create(pager), 1);
    const std::int64_t values[] = {50, 10, 30, 20, 40};
    for (std::size_t i = 0; i < 5; ++i) index.insert(Value::integer(values[i]), static_cast<std::int64_t>(i + 1));

    std::vector<std::int64_t> seen;
    index.seek_range(nullptr, false, nullptr, false, [&](std::int64_t rowid) {
        seen.push_back(rowid);
        return true;
    });
    // Rowids come back ordered by their indexed value, not by rowid.
    REQUIRE_EQ(seen[0], static_cast<std::int64_t>(2));  // value 10
    REQUIRE_EQ(seen[4], static_cast<std::int64_t>(1));  // value 50
}

TEST(Index, CountingStopsAtTheCap) {
    Pager pager(Pager::kMemoryPath);
    Index index("i", "t", "c", pager, BTree::create(pager), 1);
    for (std::int64_t i = 1; i <= 100; ++i) index.insert(Value::integer(7), i);

    REQUIRE_EQ(index.count_equal(Value::integer(7), 1000), static_cast<std::size_t>(100));
    REQUIRE_EQ(index.count_equal(Value::integer(7), 10), static_cast<std::size_t>(10));
    REQUIRE_EQ(index.count_equal(Value::integer(8), 10), static_cast<std::size_t>(0));
}

TEST(Index, EraseRemovesOneEntryNotTheWholeValue) {
    Pager pager(Pager::kMemoryPath);
    Index index("i", "t", "c", pager, BTree::create(pager), 1);
    index.insert(Value::integer(5), 1);
    index.insert(Value::integer(5), 2);

    REQUIRE(index.erase(Value::integer(5), 1));
    REQUIRE(!index.erase(Value::integer(5), 1));
    const std::vector<std::int64_t> left = equal_rowids(index, Value::integer(5));
    REQUIRE_EQ(left.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(left[0], static_cast<std::int64_t>(2));
}

TEST(Index, HandlesManyDuplicatesAcrossPageSplits) {
    Pager pager(Pager::kMemoryPath);
    Index index("i", "t", "c", pager, BTree::create(pager), 1);
    for (std::int64_t i = 1; i <= 5000; ++i) {
        index.insert(Value::integer(i % 5), i);  // five values, a thousand rows each
    }
    REQUIRE(index.height() > 1);
    for (std::int64_t v = 0; v < 5; ++v) {
        REQUIRE_EQ(equal_rowids(index, Value::integer(v)).size(),
                   static_cast<std::size_t>(1000));
    }
}
