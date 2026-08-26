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

namespace {

// A table with NULLs in both a numeric and a grouping column.
Database sales() {
    Database db;
    run(db,
        "CREATE TABLE sales (region TEXT, amount REAL, units INTEGER);"
        "INSERT INTO sales VALUES ('east', 100.0, 3);"
        "INSERT INTO sales VALUES ('east', 50.5, 1);"
        "INSERT INTO sales VALUES ('west', 200.0, 7);"
        "INSERT INTO sales VALUES ('west', NULL, 2);"
        "INSERT INTO sales VALUES ('north', 75.0, NULL);");
    return db;
}

}  // namespace

TEST(Aggregate, CountStarCountsRowsIncludingNulls) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM sales").rows[0][0], Value::integer(5));
}

TEST(Aggregate, CountOfAnExpressionSkipsNulls) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT COUNT(amount) FROM sales").rows[0][0], Value::integer(4));
    REQUIRE_EQ(run(db, "SELECT COUNT(units) FROM sales").rows[0][0], Value::integer(4));
}

TEST(Aggregate, SumIgnoresNullsAndStaysIntegralWhenItCan) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT SUM(units) FROM sales").rows[0][0], Value::integer(13));
    REQUIRE_EQ(run(db, "SELECT SUM(amount) FROM sales").rows[0][0], Value::real(425.5));
}

TEST(Aggregate, AvgDividesByTheNonNullCount) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT AVG(amount) FROM sales").rows[0][0], Value::real(106.375));
}

TEST(Aggregate, MinAndMaxIgnoreNulls) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT MIN(amount) FROM sales").rows[0][0], Value::real(50.5));
    REQUIRE_EQ(run(db, "SELECT MAX(amount) FROM sales").rows[0][0], Value::real(200.0));
}

TEST(Aggregate, MinAndMaxWorkOnText) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT MIN(region) FROM sales").rows[0][0], Value::text("east"));
    REQUIRE_EQ(run(db, "SELECT MAX(region) FROM sales").rows[0][0], Value::text("west"));
}

TEST(Aggregate, OverAnEmptyTableCountIsZeroAndTheRestAreNull) {
    Database db;
    run(db, "CREATE TABLE t (a INTEGER);");
    const ResultSet r = run(db, "SELECT COUNT(*), COUNT(a), SUM(a), AVG(a), MIN(a) FROM t");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(1));  // still exactly one row
    REQUIRE_EQ(r.rows[0][0], Value::integer(0));
    REQUIRE_EQ(r.rows[0][1], Value::integer(0));
    REQUIRE(r.rows[0][2].is_null());
    REQUIRE(r.rows[0][3].is_null());
    REQUIRE(r.rows[0][4].is_null());
}

TEST(Aggregate, RespectsWhere) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM sales WHERE region = 'east'").rows[0][0],
               Value::integer(2));
}

TEST(Aggregate, CanBeCombinedInAnExpression) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT SUM(units) * 2 FROM sales").rows[0][0], Value::integer(26));
    REQUIRE_EQ(run(db, "SELECT MAX(units) - MIN(units) FROM sales").rows[0][0],
               Value::integer(6));
}

TEST(Aggregate, UnaliasedColumnIsNamedAfterTheCall) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM sales").column_names[0], std::string("COUNT(*)"));
}

TEST(GroupBy, SplitsRowsByKeyInFirstSeenOrder) {
    Database db = sales();
    const ResultSet r = run(db, "SELECT region, COUNT(*) FROM sales GROUP BY region");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(r.rows[0][0], Value::text("east"));
    REQUIRE_EQ(r.rows[0][1], Value::integer(2));
    REQUIRE_EQ(r.rows[1][0], Value::text("west"));
    REQUIRE_EQ(r.rows[2][0], Value::text("north"));
    REQUIRE_EQ(r.rows[2][1], Value::integer(1));
}

TEST(GroupBy, AggregatesAreComputedPerGroup) {
    Database db = sales();
    const ResultSet r =
        run(db, "SELECT region, SUM(amount) FROM sales GROUP BY region ORDER BY region");
    REQUIRE_EQ(r.rows[0][0], Value::text("east"));
    REQUIRE_EQ(r.rows[0][1], Value::real(150.5));
    REQUIRE_EQ(r.rows[1][0], Value::text("north"));
    REQUIRE_EQ(r.rows[1][1], Value::real(75.0));
    REQUIRE_EQ(r.rows[2][0], Value::text("west"));
    REQUIRE_EQ(r.rows[2][1], Value::real(200.0));  // the NULL amount is skipped
}

TEST(GroupBy, GroupsNullKeysTogether) {
    Database db;
    run(db,
        "CREATE TABLE t (k TEXT, v INTEGER);"
        "INSERT INTO t VALUES (NULL, 1), ('a', 2), (NULL, 3);");
    const ResultSet r = run(db, "SELECT k, SUM(v) FROM t GROUP BY k ORDER BY k");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(2));
    REQUIRE(r.rows[0][0].is_null());          // NULLs sort first
    REQUIRE_EQ(r.rows[0][1], Value::integer(4));  // and group together, unlike '='
}

TEST(GroupBy, SupportsMultipleKeys) {
    Database db;
    run(db,
        "CREATE TABLE t (a INTEGER, b INTEGER, v INTEGER);"
        "INSERT INTO t VALUES (1,1,10), (1,2,20), (1,1,5), (2,1,7);");
    const ResultSet r = run(db, "SELECT a, b, SUM(v) FROM t GROUP BY a, b ORDER BY a, b");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(r.rows[0][2], Value::integer(15));
    REQUIRE_EQ(r.rows[1][2], Value::integer(20));
    REQUIRE_EQ(r.rows[2][2], Value::integer(7));
}

TEST(GroupBy, GroupsOnComputedKeys) {
    Database db;
    run(db,
        "CREATE TABLE t (n INTEGER);"
        "INSERT INTO t VALUES (1), (2), (3), (4), (5);");
    const ResultSet r = run(db, "SELECT n % 2, COUNT(*) FROM t GROUP BY n % 2 ORDER BY n % 2");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(r.rows[0][1], Value::integer(2));  // evens
    REQUIRE_EQ(r.rows[1][1], Value::integer(3));  // odds
}

TEST(Having, FiltersGroupsNotRows) {
    Database db = sales();
    const ResultSet r =
        run(db, "SELECT region FROM sales GROUP BY region HAVING SUM(amount) > 100 "
                "ORDER BY region");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(r.rows[0][0], Value::text("east"));
    REQUIRE_EQ(r.rows[1][0], Value::text("west"));
}

TEST(Having, CanReferenceAGroupingKey) {
    Database db = sales();
    const ResultSet r =
        run(db, "SELECT region, COUNT(*) FROM sales GROUP BY region HAVING region <> 'east'");
    REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(2));
}

TEST(Having, WithoutGroupByFiltersTheSingleGroup) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM sales HAVING COUNT(*) > 99").rows.size(),
               static_cast<std::size_t>(0));
    REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM sales HAVING COUNT(*) > 1").rows.size(),
               static_cast<std::size_t>(1));
}

TEST(OrderBy, CanNameASelectListAlias) {
    Database db = sales();
    const ResultSet r = run(db, "SELECT region, SUM(amount) AS total FROM sales "
                                "GROUP BY region ORDER BY total DESC");
    REQUIRE_EQ(r.rows[0][0], Value::text("west"));
    REQUIRE_EQ(r.rows[2][0], Value::text("north"));
}

TEST(OrderBy, CanNameAnOutputPosition) {
    Database db = sales();
    const ResultSet r = run(db, "SELECT region, SUM(amount) FROM sales GROUP BY region "
                                "ORDER BY 2 DESC");
    REQUIRE_EQ(r.rows[0][0], Value::text("west"));
}

TEST(OrderBy, RejectsAPositionOutOfRange) {
    Database db = sales();
    REQUIRE_THROWS_AS(run(db, "SELECT region FROM sales ORDER BY 7"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT region FROM sales ORDER BY 0"), DbError);
}

TEST(OrderBy, CanSortByAnAggregateNotInTheSelectList) {
    Database db = sales();
    const ResultSet r =
        run(db, "SELECT region FROM sales GROUP BY region ORDER BY COUNT(*) DESC, region");
    REQUIRE_EQ(r.rows[0][0], Value::text("east"));  // two rows, ties broken by name
    REQUIRE_EQ(r.rows[2][0], Value::text("north"));
}

TEST(ScalarFunction, AbsKeepsIntegerness) {
    Database db;
    REQUIRE_EQ(run(db, "SELECT ABS(-5)").rows[0][0], Value::integer(5));
    REQUIRE_EQ(run(db, "SELECT ABS(-2.5)").rows[0][0], Value::real(2.5));
    REQUIRE(run(db, "SELECT ABS(NULL)").rows[0][0].is_null());
}

TEST(ScalarFunction, LengthUpperAndLower) {
    Database db;
    REQUIRE_EQ(run(db, "SELECT LENGTH('hello')").rows[0][0], Value::integer(5));
    REQUIRE_EQ(run(db, "SELECT UPPER('MiniDB')").rows[0][0], Value::text("MINIDB"));
    REQUIRE_EQ(run(db, "SELECT LOWER('MiniDB')").rows[0][0], Value::text("minidb"));
    REQUIRE(run(db, "SELECT UPPER(NULL)").rows[0][0].is_null());
}

TEST(ScalarFunction, RoundHonoursTheDigitArgument) {
    Database db;
    REQUIRE_EQ(run(db, "SELECT ROUND(3.14159)").rows[0][0], Value::real(3.0));
    REQUIRE_EQ(run(db, "SELECT ROUND(3.14159, 2)").rows[0][0], Value::real(3.14));
}

TEST(ScalarFunction, CoalesceAndIfnullPickTheFirstNonNull) {
    Database db;
    REQUIRE_EQ(run(db, "SELECT COALESCE(NULL, NULL, 3)").rows[0][0], Value::integer(3));
    REQUIRE_EQ(run(db, "SELECT IFNULL(NULL, 'fallback')").rows[0][0], Value::text("fallback"));
    REQUIRE_EQ(run(db, "SELECT IFNULL(1, 2)").rows[0][0], Value::integer(1));
    REQUIRE(run(db, "SELECT COALESCE(NULL, NULL)").rows[0][0].is_null());
}

TEST(ScalarFunction, WorksInsideWhereAndAggregates) {
    Database db = sales();
    REQUIRE_EQ(run(db, "SELECT COUNT(*) FROM sales WHERE LENGTH(region) = 4").rows[0][0],
               Value::integer(4));
    REQUIRE_EQ(run(db, "SELECT SUM(ABS(units)) FROM sales").rows[0][0], Value::integer(13));
}

TEST(GroupingErrors, RejectsABareColumnAlongsideAnAggregate) {
    Database db = sales();
    REQUIRE_THROWS_AS(run(db, "SELECT region, COUNT(*) FROM sales"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT amount FROM sales GROUP BY region"), DbError);
}

TEST(GroupingErrors, RejectsSelectStarWithGrouping) {
    Database db = sales();
    REQUIRE_THROWS_AS(run(db, "SELECT * FROM sales GROUP BY region"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT * , COUNT(*) FROM sales"), DbError);
}

TEST(GroupingErrors, RejectsAggregatesInWhereAndGroupBy) {
    Database db = sales();
    REQUIRE_THROWS_AS(run(db, "SELECT region FROM sales WHERE COUNT(*) > 1"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT COUNT(*) FROM sales GROUP BY SUM(units)"), DbError);
}

TEST(GroupingErrors, RejectsNestedAggregates) {
    Database db = sales();
    REQUIRE_THROWS_AS(run(db, "SELECT SUM(COUNT(units)) FROM sales"), DbError);
}

TEST(FunctionErrors, RejectsUnknownFunctionsAndBadArity) {
    Database db = sales();
    REQUIRE_THROWS_AS(run(db, "SELECT nosuchfunc(1)"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT LENGTH()"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT COUNT(region, amount) FROM sales"), DbError);
    REQUIRE_THROWS_AS(run(db, "SELECT SUM(*) FROM sales"), DbError);
}

TEST(FunctionErrors, RejectsSumOverText) {
    Database db = sales();
    REQUIRE_THROWS_AS(run(db, "SELECT SUM(region) FROM sales"), DbError);
}

TEST(FunctionErrors, RejectsAggregatesInInsertValues) {
    Database db = sales();
    REQUIRE_THROWS_AS(run(db, "INSERT INTO sales VALUES (COUNT(*), 1.0, 1)"), DbError);
}
