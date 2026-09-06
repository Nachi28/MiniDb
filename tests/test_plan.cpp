#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "minidb/database.hpp"
#include "minidb/diagnostic.hpp"
#include "minidb/executor.hpp"
#include "minidb/parser.hpp"
#include "minidb/plan.hpp"
#include "test_framework.hpp"

using namespace minidb;

namespace {

ResultSet run(Database& db, const std::string& sql) {
    Parser parser(sql);
    Executor executor(db);
    ResultSet last;
    const std::vector<Statement> statements = parser.parse_program();
    for (const Statement& stmt : statements) last = executor.execute(stmt);
    return last;
}

std::string explain(Database& db, const std::string& query) {
    const ResultSet r = run(db, "EXPLAIN " + query);
    std::string text;
    for (const Row& row : r.rows) {
        if (!text.empty()) text += "\n";
        text += row[0].to_string();
    }
    return text;
}

bool mentions(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A table where `bucket` has few distinct values (unselective) and `unique_id`
// has one row each (very selective), so the planner has a real choice to make.
Database populated(int rows) {
    Database db;
    run(db, "CREATE TABLE t (unique_id INTEGER, bucket INTEGER, label TEXT);");
    std::string sql = "INSERT INTO t VALUES ";
    for (int i = 1; i <= rows; ++i) {
        if (i > 1) sql += ", ";
        sql += "(" + std::to_string(i) + ", " + std::to_string(i % 4) + ", 'row-" +
               std::to_string(i % 50) + "')";
    }
    run(db, sql + ";");
    return db;
}

}  // namespace

TEST(Plan, WithoutAnIndexEverythingIsAScan) {
    Database db = populated(400);
    REQUIRE(mentions(explain(db, "SELECT * FROM t WHERE unique_id = 5"), "SCAN TABLE"));
}

TEST(Plan, ASelectiveEqualityUsesTheIndex) {
    Database db = populated(400);
    run(db, "CREATE INDEX idx_uid ON t (unique_id);");
    const std::string plan = explain(db, "SELECT * FROM t WHERE unique_id = 5");
    REQUIRE(mentions(plan, "SEEK INDEX idx_uid"));
    REQUIRE(mentions(plan, "rows=1"));  // probed, not guessed
}

TEST(Plan, AnUnselectivePredicateFallsBackToTheScan) {
    Database db = populated(400);
    run(db, "CREATE INDEX idx_bucket ON t (bucket);");
    // A quarter of the table matches; reaching all of it through the index costs
    // more than reading the table straight through.
    REQUIRE(mentions(explain(db, "SELECT * FROM t WHERE bucket = 1"), "SCAN TABLE"));
}

TEST(Plan, RangesUseTheIndexAndReportBothBounds) {
    Database db = populated(400);
    run(db, "CREATE INDEX idx_uid ON t (unique_id);");
    const std::string plan =
        explain(db, "SELECT * FROM t WHERE unique_id >= 10 AND unique_id < 20");
    REQUIRE(mentions(plan, "RANGE INDEX idx_uid"));
    REQUIRE(mentions(plan, "rows=10"));
}

TEST(Plan, ALiteralOnTheLeftIsRecognisedToo) {
    Database db = populated(400);
    run(db, "CREATE INDEX idx_uid ON t (unique_id);");
    REQUIRE(mentions(explain(db, "SELECT * FROM t WHERE 5 = unique_id"), "SEEK INDEX"));
}

TEST(Plan, OrPredicatesCannotUseAnIndex) {
    Database db = populated(400);
    run(db, "CREATE INDEX idx_uid ON t (unique_id);");
    // Under an OR, a row failing one side may still qualify, so no single range
    // covers the predicate.
    REQUIRE(mentions(explain(db, "SELECT * FROM t WHERE unique_id = 5 OR label = 'x'"),
                     "SCAN TABLE"));
}

TEST(Plan, AnIndexOnAnotherColumnIsIgnored) {
    Database db = populated(400);
    run(db, "CREATE INDEX idx_uid ON t (unique_id);");
    REQUIRE(mentions(explain(db, "SELECT * FROM t WHERE label = 'row-3'"), "SCAN TABLE"));
}

TEST(Plan, ExplainDescribesTheWholePipeline) {
    Database db = populated(400);
    run(db, "CREATE INDEX idx_uid ON t (unique_id);");
    const std::string plan =
        explain(db, "SELECT bucket, COUNT(*) FROM t WHERE unique_id >= 10 AND unique_id < 30 "
                    "GROUP BY bucket HAVING COUNT(*) > 1 ORDER BY 2 DESC LIMIT 2");
    REQUIRE(mentions(plan, "RANGE INDEX"));
    REQUIRE(mentions(plan, "FILTER"));
    REQUIRE(mentions(plan, "GROUP BY"));
    REQUIRE(mentions(plan, "AGGREGATE"));
    REQUIRE(mentions(plan, "HAVING"));
    REQUIRE(mentions(plan, "SORT BY"));
    REQUIRE(mentions(plan, "LIMIT 2"));
}

TEST(Plan, ExplainDoesNotRunTheQuery) {
    Database db = populated(50);
    const ResultSet r = run(db, "EXPLAIN SELECT * FROM t");
    REQUIRE_EQ(r.column_names.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(r.column_names[0], std::string("plan"));
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(1));
}

namespace {

struct Lcg {
    std::uint64_t state;
    explicit Lcg(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 16;
    }
};

// Two databases holding identical rows, one indexed and one not. Whatever the
// planner decides, the answers have to match.
void build_pair(Database& plain, Database& indexed, int rows, std::uint64_t seed) {
    const char* schema = "CREATE TABLE t (n INTEGER, word TEXT, ratio REAL);";
    run(plain, schema);
    run(indexed, schema);

    Lcg rng(seed);
    std::string sql = "INSERT INTO t VALUES ";
    for (int i = 0; i < rows; ++i) {
        if (i > 0) sql += ", ";
        const std::int64_t n = static_cast<std::int64_t>(rng.next() % 500);
        const std::int64_t w = static_cast<std::int64_t>(rng.next() % 26);
        const std::int64_t r = static_cast<std::int64_t>(rng.next() % 1000);
        sql += "(" + std::to_string(n) + ", '" +
               std::string(1, static_cast<char>('a' + w)) + "', " +
               std::to_string(r) + ".5)";
    }
    sql += ";";
    run(plain, sql);
    run(indexed, sql);

    run(indexed, "CREATE INDEX idx_n ON t (n);");
    run(indexed, "CREATE INDEX idx_word ON t (word);");
    run(indexed, "CREATE INDEX idx_ratio ON t (ratio);");
}

std::string normalise(const ResultSet& result) {
    // ORDER BY is not part of every query below, and an index scan returns rows
    // in index order rather than rowid order, so compare as multisets.
    std::vector<std::string> lines;
    for (const Row& row : result.rows) {
        std::string line;
        for (const Value& value : row) line += value.to_sql() + "|";
        lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    std::string all;
    for (const std::string& line : lines) all += line + ";";
    return all;
}

}  // namespace

TEST(Plan, IndexedAndUnindexedDatabasesAgreeOnEveryQuery) {
    Database plain;
    Database indexed;
    build_pair(plain, indexed, 800, 20260827);

    const char* queries[] = {
        "SELECT * FROM t WHERE n = 42",
        "SELECT * FROM t WHERE n <> 42",
        "SELECT * FROM t WHERE n > 400",
        "SELECT * FROM t WHERE n >= 100 AND n < 120",
        "SELECT * FROM t WHERE n <= 3",
        "SELECT * FROM t WHERE word = 'q'",
        "SELECT * FROM t WHERE word > 'w'",
        "SELECT * FROM t WHERE ratio = 250.5",
        "SELECT * FROM t WHERE ratio >= 900.5",
        "SELECT * FROM t WHERE n = 7 AND word = 'a'",
        "SELECT * FROM t WHERE n = 7 OR word = 'a'",
        "SELECT * FROM t WHERE n IS NULL",
        "SELECT * FROM t WHERE n = 999999",
        "SELECT COUNT(*), SUM(n), MIN(ratio), MAX(word) FROM t WHERE n < 50",
        "SELECT word, COUNT(*) FROM t WHERE n >= 200 GROUP BY word",
        "SELECT n FROM t WHERE n > 100 ORDER BY n LIMIT 20",
        "SELECT * FROM t WHERE NOT n = 5",
        "SELECT * FROM t WHERE n + 0 = 42",
    };

    for (const char* query : queries) {
        const std::string a = normalise(run(plain, query));
        const std::string b = normalise(run(indexed, query));
        if (a != b) {
            std::ostringstream message;
            message << "    plans disagree for: " << query;
            throw ::minitest::AssertionFailure{message.str()};
        }
    }
}

TEST(Plan, IndexesStayCorrectAsRowsAreAdded) {
    Database db;
    run(db, "CREATE TABLE t (n INTEGER);");
    run(db, "INSERT INTO t VALUES (1), (2), (3);");
    run(db, "CREATE INDEX idx_n ON t (n);");
    // Backfilled from the three that were already there, then maintained.
    run(db, "INSERT INTO t VALUES (2), (4), (2);");

    REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM t WHERE n = 2").rows[0][0], Value::integer(3));
    REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM t WHERE n = 4").rows[0][0], Value::integer(1));
    REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM t").rows[0][0], Value::integer(6));
}

TEST(Plan, IndexesSurviveReopeningAndKeepWorking) {
    const std::string path = "minidb_plan_reopen.db";
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    {
        Database db(path);
        run(db, "CREATE TABLE t (n INTEGER);");
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = 1; i <= 500; ++i) {
            if (i > 1) sql += ", ";
            sql += "(" + std::to_string(i) + ")";
        }
        run(db, sql + ";");
        run(db, "CREATE INDEX idx_n ON t (n);");
    }
    {
        Database db(path);
        REQUIRE_EQ(db.index_count(), static_cast<std::size_t>(1));
        REQUIRE(mentions(explain(db, "SELECT * FROM t WHERE n = 250"), "SEEK INDEX"));
        REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM t WHERE n = 250").rows[0][0],
                   Value::integer(1));
        run(db, "INSERT INTO t VALUES (250);");
        REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM t WHERE n = 250").rows[0][0],
                   Value::integer(2));
    }
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
}

TEST(Plan, CreateIndexRejectsBadArguments) {
    Database db = populated(20);
    run(db, "CREATE INDEX idx_uid ON t (unique_id);");
    REQUIRE_THROWS_AS(run(db, "CREATE INDEX idx_uid ON t (bucket);"), DbError);
    REQUIRE_THROWS_AS(run(db, "CREATE INDEX idx_two ON t (unique_id);"), DbError);
    REQUIRE_THROWS_AS(run(db, "CREATE INDEX idx_x ON nosuch (a);"), DbError);
    REQUIRE_THROWS_AS(run(db, "CREATE INDEX idx_y ON t (nosuchcol);"), DbError);
    run(db, "CREATE INDEX IF NOT EXISTS idx_uid ON t (bucket);");  // tolerated
}

namespace {

std::string analyze(Database& db, const std::string& query) {
    const ResultSet r = run(db, "EXPLAIN ANALYZE " + query);
    std::string text;
    for (const Row& row : r.rows) {
        if (!text.empty()) text += "\n";
        text += row[0].to_string();
    }
    return text;
}

}  // namespace

TEST(Analyze, ReportsThePlanRatherThanTheRows) {
    Database db = populated(400);
    const ResultSet r = run(db, "EXPLAIN ANALYZE SELECT * FROM t");
    REQUIRE_EQ(r.column_names.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(r.column_names[0], std::string("plan"));
    // A report, not 400 rows of data.
    REQUIRE(r.rows.size() < static_cast<std::size_t>(20));
    REQUIRE(mentions(analyze(db, "SELECT * FROM t"), "TOTAL"));
}

TEST(Analyze, PlainExplainDoesNotRunTheQuery) {
    Database db = populated(400);
    const std::string plain = explain(db, "SELECT * FROM t");
    REQUIRE(!mentions(plain, "TOTAL"));
    REQUIRE(!mentions(plain, "actual"));
    REQUIRE(!mentions(plain, "ESTIMATE"));
}

TEST(Analyze, CountsRowsTheAccessPathActuallyProduced) {
    Database db = populated(400);
    const std::string plan = analyze(db, "SELECT * FROM t WHERE bucket = 1");
    REQUIRE(mentions(plan, "SCAN TABLE"));
    REQUIRE(mentions(plan, "actual: 400 row(s) from the access path"));
}

TEST(Analyze, ShowsHowManyRowsTheFilterRemoved) {
    Database db = populated(400);
    // bucket cycles 0..3, so a quarter of the table survives.
    REQUIRE(mentions(analyze(db, "SELECT * FROM t WHERE bucket = 1"),
                     "400 in, 100 out"));
}

TEST(Analyze, AnIndexSeekReportsOnlyTheRowsItTouched) {
    Database db = populated(400);
    run(db, "CREATE INDEX idx_uid ON t (unique_id);");
    const std::string plan = analyze(db, "SELECT * FROM t WHERE unique_id = 5");
    REQUIRE(mentions(plan, "SEEK INDEX idx_uid"));
    // One row reached, not four hundred: the whole point of the index.
    REQUIRE(mentions(plan, "actual: 1 row(s) from the access path"));
    REQUIRE(mentions(plan, "1 in, 1 out"));
}

TEST(Analyze, GradesItsOwnEstimate) {
    Database db = populated(400);
    run(db, "CREATE INDEX idx_uid ON t (unique_id);");
    // The planner probes rather than guessing, so a healthy database should
    // always report an exact estimate. Anything else means a row count or an
    // index has drifted out of step with the table.
    REQUIRE(mentions(analyze(db, "SELECT * FROM t WHERE unique_id = 5"),
                     "predicted 1, actual 1 (exact)"));
    REQUIRE(mentions(analyze(db, "SELECT * FROM t WHERE bucket = 2"),
                     "predicted 400, actual 400 (exact)"));
}

TEST(Analyze, ReportsEveryStageOfThePipeline) {
    Database db = populated(400);
    const std::string plan = analyze(db,
        "SELECT bucket, COUNT(*) AS n FROM t WHERE unique_id > 100 "
        "GROUP BY bucket HAVING COUNT(*) > 1 ORDER BY n DESC LIMIT 2");
    REQUIRE(mentions(plan, "FILTER"));
    REQUIRE(mentions(plan, "GROUP BY"));
    REQUIRE(mentions(plan, "group(s)"));
    REQUIRE(mentions(plan, "AGGREGATE"));
    REQUIRE(mentions(plan, "HAVING"));
    REQUIRE(mentions(plan, "SORT BY"));
    REQUIRE(mentions(plan, "LIMIT 2"));
    REQUIRE(mentions(plan, "TOTAL"));
}

TEST(Analyze, WorksWithoutAFromClause) {
    Database db;
    const std::string plan = analyze(db, "SELECT 1 + 2");
    REQUIRE(mentions(plan, "CONSTANT ROW"));
    REQUIRE(mentions(plan, "TOTAL"));
    REQUIRE(!mentions(plan, "ESTIMATE"));  // nothing was estimated
}

TEST(Analyze, AnalyseIsAcceptedAsASpelling) {
    Database db = populated(50);
    REQUIRE(mentions(analyze(db, "SELECT * FROM t"), "TOTAL"));
    const ResultSet r = run(db, "EXPLAIN ANALYSE SELECT * FROM t");
    REQUIRE_EQ(r.column_names[0], std::string("plan"));
}
