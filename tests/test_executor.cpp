#include "minidb/executor.hpp"

#include <string>

#include "minidb/database.hpp"
#include "minidb/diagnostic.hpp"
#include "minidb/parser.hpp"
#include "test_framework.hpp"

using namespace minidb;

namespace {

// Runs every statement in `sql` and returns the last result set.
ResultSet run(Database& db, const std::string& sql) {
    Parser parser(sql);
    Executor executor(db);
    ResultSet last;
    const std::vector<Statement> statements = parser.parse_program();
    for (const Statement& stmt : statements) last = executor.execute(stmt);
    return last;
}

// A table with a NULL in it, so the null-handling tests have something to bite on.
Database seeded() {
    Database db;
    run(db,
        "CREATE TABLE players (id INTEGER, name TEXT, score REAL);"
        "INSERT INTO players VALUES (1, 'ada', 90.5);"
        "INSERT INTO players VALUES (2, 'linus', 72.0);"
        "INSERT INTO players VALUES (3, 'grace', NULL);");
    return db;
}

}  // namespace

TEST(Executor, SelectStarReturnsEveryColumnAndRow) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT * FROM players");
    REQUIRE(r.is_query);
    REQUIRE_EQ(r.column_names.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(r.column_names[1], std::string("name"));
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(r.rows[0][1], Value::text("ada"));
}

TEST(Executor, WhereFiltersRows) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT name FROM players WHERE id > 1");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(r.rows[0][0], Value::text("linus"));
}

TEST(Executor, ProjectsComputedColumnsWithAliases) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT name, id * 10 AS ten FROM players WHERE id = 2");
    REQUIRE_EQ(r.column_names[1], std::string("ten"));
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(r.rows[0][1], Value::integer(20));
}

TEST(Executor, UnaliasedExpressionsAreNamedAfterTheirShape) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT id + 1 FROM players WHERE id = 1");
    REQUIRE_EQ(r.column_names[0], std::string("(id + 1)"));
}

TEST(Executor, OrderByAscendingAndDescending) {
    Database db = seeded();
    const ResultSet asc = run(db, "SELECT name FROM players ORDER BY name");
    REQUIRE_EQ(asc.rows[0][0], Value::text("ada"));
    REQUIRE_EQ(asc.rows[2][0], Value::text("linus"));

    const ResultSet desc = run(db, "SELECT name FROM players ORDER BY name DESC");
    REQUIRE_EQ(desc.rows[0][0], Value::text("linus"));
}

TEST(Executor, OrderBySortsNullsFirst) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT name FROM players ORDER BY score");
    REQUIRE_EQ(r.rows[0][0], Value::text("grace"));  // the NULL score
}

TEST(Executor, LimitTruncatesResults) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT name FROM players ORDER BY id LIMIT 2");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(r.rows[1][0], Value::text("linus"));
}

TEST(Executor, LimitLargerThanTheTableIsHarmless) {
    Database db = seeded();
    REQUIRE_EQ(run(db, "SELECT * FROM players LIMIT 99").rows.size(),
               static_cast<std::size_t>(3));
}

TEST(Executor, EqualityAgainstNullMatchesNothing) {
    Database db = seeded();
    REQUIRE_EQ(run(db, "SELECT * FROM players WHERE score = NULL").rows.size(),
               static_cast<std::size_t>(0));
    REQUIRE_EQ(run(db, "SELECT * FROM players WHERE score <> NULL").rows.size(),
               static_cast<std::size_t>(0));
}

TEST(Executor, IsNullIsTheNullSafeTest) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT name FROM players WHERE score IS NULL");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(r.rows[0][0], Value::text("grace"));

    REQUIRE_EQ(run(db, "SELECT * FROM players WHERE score IS NOT NULL").rows.size(),
               static_cast<std::size_t>(2));
}

TEST(Executor, FalseAndNullIsFalseSoTheRowIsDropped) {
    Database db = seeded();
    // score IS NULL for grace, so `score > 0` is NULL there; combined with a
    // false conjunct the whole predicate must be false for every row.
    REQUIRE_EQ(run(db, "SELECT * FROM players WHERE score > 0 AND id > 99").rows.size(),
               static_cast<std::size_t>(0));
}

TEST(Executor, TrueOrNullIsTrue) {
    Database db = seeded();
    REQUIRE_EQ(run(db, "SELECT * FROM players WHERE id = 3 OR score > 80").rows.size(),
               static_cast<std::size_t>(2));
}

TEST(Executor, ArithmeticOnNullYieldsNull) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT score + 1 FROM players WHERE id = 3");
    REQUIRE(r.rows[0][0].is_null());
}

TEST(Executor, SelectWithoutFromEvaluatesConstants) {
    Database db;
    const ResultSet r = run(db, "SELECT 1 + 2 * 3");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(r.rows[0][0], Value::integer(7));
}

TEST(Executor, IntegerDivisionTruncatesButRealDivisionDoesNot) {
    Database db;
    REQUIRE_EQ(run(db, "SELECT 7 / 2").rows[0][0], Value::integer(3));
    REQUIRE_EQ(run(db, "SELECT 7.0 / 2").rows[0][0], Value::real(3.5));
}

TEST(Executor, ConcatenationJoinsText) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT name || '!' FROM players WHERE id = 1");
    REQUIRE_EQ(r.rows[0][0], Value::text("ada!"));
}

TEST(Executor, InsertRespectsANamedColumnList) {
    Database db;
    const ResultSet r = run(db,
                            "CREATE TABLE t (a INTEGER, b TEXT);"
                            "INSERT INTO t (b, a) VALUES ('x', 1);"
                            "SELECT * FROM t");
    REQUIRE_EQ(r.rows[0][0], Value::integer(1));
    REQUIRE_EQ(r.rows[0][1], Value::text("x"));
}

TEST(Executor, InsertFillsOmittedColumnsWithNull) {
    Database db;
    const ResultSet r = run(db,
                            "CREATE TABLE t (a INTEGER, b TEXT);"
                            "INSERT INTO t (a) VALUES (1);"
                            "SELECT * FROM t");
    REQUIRE(r.rows[0][1].is_null());
}

TEST(Executor, IfNotExistsSuppressesTheDuplicateError) {
    Database db;
    run(db, "CREATE TABLE t (a INT);");
    run(db, "CREATE TABLE IF NOT EXISTS t (a INT);");
    REQUIRE_EQ(db.table_count(), static_cast<std::size_t>(1));
}

TEST(Executor, RejectsDuplicateTable) {
    Database db;
    run(db, "CREATE TABLE t (a INT);");
    REQUIRE_THROWS_AS(run(db, "CREATE TABLE t (a INT);"), DbError);
}

TEST(Executor, RejectsDuplicateColumnName) {
    Database db;
    REQUIRE_THROWS_AS(run(db, "CREATE TABLE t (a INT, A TEXT);"), DbError);
}

TEST(Executor, RejectsUnknownTable) {
    Database db;
    REQUIRE_THROWS_AS(run(db, "SELECT * FROM nope"), DbError);
    REQUIRE_THROWS_AS(run(db, "INSERT INTO nope VALUES (1)"), DbError);
}

TEST(Executor, RejectsUnknownColumn) {
    Database db = seeded();
    REQUIRE_THROWS_AS(run(db, "SELECT nope FROM players"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT * FROM players WHERE nope = 1"), DbError);
}

TEST(Executor, RejectsWrongNumberOfValues) {
    Database db = seeded();
    REQUIRE_THROWS_AS(run(db, "INSERT INTO players VALUES (1, 'x')"), DbError);
}

TEST(Executor, RejectsTypeMismatchOnInsert) {
    Database db = seeded();
    REQUIRE_THROWS_AS(run(db, "INSERT INTO players VALUES ('one', 'x', 1.0)"), DbError);
}

TEST(Executor, RejectsDivisionByZero) {
    Database db;
    REQUIRE_THROWS_AS(run(db, "SELECT 1 / 0"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT 1 % 0"), DbError);
}

TEST(Executor, RejectsArithmeticOnText) {
    Database db = seeded();
    REQUIRE_THROWS_AS(run(db, "SELECT name * 2 FROM players"), DbError);
}

TEST(Executor, TableAndColumnNamesAreCaseInsensitive) {
    Database db = seeded();
    const ResultSet r = run(db, "SELECT NAME FROM PLAYERS WHERE ID = 1");
    REQUIRE_EQ(r.rows[0][0], Value::text("ada"));
}

TEST(Executor, ReportsHowManyRowsWereInserted) {
    Database db;
    const ResultSet r = run(db,
                            "CREATE TABLE t (a INT);"
                            "INSERT INTO t VALUES (1), (2), (3);");
    REQUIRE(!r.is_query);
    REQUIRE_EQ(r.message, std::string("3 row(s) inserted"));
}

TEST(Executor, RejectsUnknownColumnEvenOnAnEmptyTable) {
    // Regression: projection only evaluates expressions once per row, so an
    // empty table used to let a bad column name through silently.
    Database db;
    run(db, "CREATE TABLE t (a INTEGER);");
    REQUIRE_THROWS_AS(run(db, "SELECT nope FROM t"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT * FROM t WHERE nope = 1"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT a FROM t ORDER BY nope"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT -nope FROM t"), DbError);
}

TEST(Executor, StillAcceptsValidColumnsOnAnEmptyTable) {
    Database db;
    run(db, "CREATE TABLE t (a INTEGER, b TEXT);");
    const ResultSet r = run(db, "SELECT b, a + 1 FROM t WHERE a > 0 ORDER BY a");
    REQUIRE(r.is_query);
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(0));
    REQUIRE_EQ(r.column_names.size(), static_cast<std::size_t>(2));
}
