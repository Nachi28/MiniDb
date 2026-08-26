#include <cstdio>
#include <string>
#include <vector>

#include "minidb/database.hpp"
#include "minidb/diagnostic.hpp"
#include "minidb/executor.hpp"
#include "minidb/parser.hpp"
#include "test_framework.hpp"

using namespace minidb;

namespace {

struct TempDb {
    std::string path;
    explicit TempDb(const std::string& name) : path("minidb_sql_" + name + ".db") {
        std::remove(path.c_str());
    }
    ~TempDb() { std::remove(path.c_str()); }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;
};

ResultSet run(Database& db, const std::string& sql) {
    Parser parser(sql);
    Executor executor(db);
    ResultSet last;
    const std::vector<Statement> statements = parser.parse_program();
    for (const Statement& stmt : statements) last = executor.execute(stmt);
    return last;
}

}  // namespace

TEST(Storage, TablesAndRowsSurviveReopening) {
    TempDb db("basic");
    {
        Database database(db.path);
        run(database,
            "CREATE TABLE t (a INTEGER, b TEXT, c REAL);"
            "INSERT INTO t VALUES (1, 'one', 1.5), (2, 'two', NULL), (3, NULL, 3.5);");
        database.flush();
    }
    {
        Database database(db.path);
        const ResultSet r = run(database, "SELECT * FROM t ORDER BY a");
        REQUIRE_EQ(r.rows.size(), static_cast<std::size_t>(3));
        REQUIRE_EQ(r.rows[0][1], Value::text("one"));
        REQUIRE(r.rows[1][2].is_null());
        REQUIRE(r.rows[2][1].is_null());
        REQUIRE_EQ(r.rows[2][2], Value::real(3.5));
    }
}

TEST(Storage, SchemaIsRecoveredFromTheCatalogue) {
    TempDb db("schema");
    {
        Database database(db.path);
        run(database, "CREATE TABLE people (id INTEGER, name TEXT, height REAL);");
        database.flush();
    }
    {
        Database database(db.path);
        const Table* table = database.find_table("people");
        REQUIRE(table != nullptr);
        REQUIRE_EQ(table->name(), std::string("people"));
        REQUIRE_EQ(table->columns().size(), static_cast<std::size_t>(3));
        REQUIRE_EQ(table->columns()[0].name, std::string("id"));
        REQUIRE(table->columns()[0].type == ValueType::Integer);
        REQUIRE(table->columns()[1].type == ValueType::Text);
        REQUIRE(table->columns()[2].type == ValueType::Real);
    }
}

TEST(Storage, SeveralTablesCoexistInOneFile) {
    TempDb db("multi");
    {
        Database database(db.path);
        run(database,
            "CREATE TABLE a (x INTEGER);"
            "CREATE TABLE b (y TEXT);"
            "CREATE TABLE c (z REAL);"
            "INSERT INTO a VALUES (1), (2);"
            "INSERT INTO b VALUES ('hello');"
            "INSERT INTO c VALUES (2.5), (3.5), (4.5);");
        database.flush();
    }
    {
        Database database(db.path);
        REQUIRE_EQ(database.table_count(), static_cast<std::size_t>(3));
        REQUIRE_EQ(run(database, "SELECT COUNT(*) FROM a").rows[0][0], Value::integer(2));
        REQUIRE_EQ(run(database, "SELECT COUNT(*) FROM b").rows[0][0], Value::integer(1));
        REQUIRE_EQ(run(database, "SELECT SUM(z) FROM c").rows[0][0], Value::real(10.5));
    }
}

TEST(Storage, InsertsContinueAfterReopeningWithoutColliding) {
    TempDb db("rowid");
    {
        Database database(db.path);
        run(database, "CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1), (2), (3);");
        database.flush();
    }
    {
        Database database(db.path);
        run(database, "INSERT INTO t VALUES (4), (5);");
        database.flush();
    }
    {
        Database database(db.path);
        // If the rowid counter had restarted, the new rows would have overwritten
        // the old ones and this would be 3 rather than 5.
        REQUIRE_EQ(run(database, "SELECT COUNT(*) FROM t").rows[0][0], Value::integer(5));
        REQUIRE_EQ(run(database, "SELECT SUM(a) FROM t").rows[0][0], Value::integer(15));
    }
}

TEST(Storage, ClosingWithoutAnExplicitFlushStillPersists) {
    TempDb db("implicit");
    {
        Database database(db.path);
        run(database, "CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (7);");
    }  // the destructor writes the catalogue and flushes the pool
    {
        Database database(db.path);
        REQUIRE_EQ(run(database, "SELECT a FROM t").rows[0][0], Value::integer(7));
    }
}

TEST(Storage, ATableLargerThanTheBufferPoolPersists) {
    TempDb db("large");
    const int n = 4000;
    {
        Database database(db.path);
        run(database, "CREATE TABLE t (id INTEGER, label TEXT);");
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = 1; i <= n; ++i) {
            if (i > 1) sql += ", ";
            sql += "(" + std::to_string(i) + ", 'row-" + std::to_string(i) + "')";
        }
        sql += ";";
        run(database, sql);
        database.flush();
    }
    {
        Database database(db.path);
        REQUIRE_EQ(run(database, "SELECT COUNT(*) FROM t").rows[0][0],
                   Value::integer(n));
        REQUIRE_EQ(run(database, "SELECT SUM(id) FROM t").rows[0][0],
                   Value::integer(static_cast<std::int64_t>(n) * (n + 1) / 2));
        const ResultSet r = run(database, "SELECT label FROM t WHERE id = 2500");
        REQUIRE_EQ(r.rows[0][0], Value::text("row-2500"));
    }
}

TEST(Storage, AnEmptyDatabaseReopensCleanly) {
    TempDb db("empty");
    { Database database(db.path); }
    {
        Database database(db.path);
        REQUIRE_EQ(database.table_count(), static_cast<std::size_t>(0));
        run(database, "CREATE TABLE t (a INTEGER);");
        REQUIRE_EQ(database.table_count(), static_cast<std::size_t>(1));
    }
}

TEST(Storage, RowsTooLargeForAPageAreRejected) {
    Database database;
    run(database, "CREATE TABLE t (blob TEXT);");
    const std::string huge(3000, 'x');
    REQUIRE_THROWS_AS(run(database, "INSERT INTO t VALUES ('" + huge + "')"), DbError);
}

TEST(Storage, InMemoryDatabasesStillWork) {
    Database database(Database::kMemory);
    run(database, "CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1), (2), (3);");
    REQUIRE_EQ(run(database, "SELECT SUM(a) FROM t").rows[0][0], Value::integer(6));
    REQUIRE_EQ(database.path(), std::string(Database::kMemory));
}
