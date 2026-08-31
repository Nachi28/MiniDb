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
    explicit TempDb(const std::string& name) : path("minidb_tx_" + name + ".db") {
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
    }
    ~TempDb() {
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
    }
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

std::int64_t count(Database& db, const std::string& table) {
    return run(db, "SELECT COUNT(*) FROM " + table).rows[0][0].as_integer();
}

}  // namespace

TEST(Transaction, RollbackUndoesInserts) {
    TempDb db("undo");
    Database database(db.path);
    run(database, "CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1), (2);");
    REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(2));

    run(database, "BEGIN; INSERT INTO t VALUES (3), (4), (5);");
    REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(5));

    run(database, "ROLLBACK;");
    REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(2));
    REQUIRE_EQ(run(database, "SELECT SUM(a) FROM t").rows[0][0], Value::integer(3));
}

TEST(Transaction, CommitKeepsTheChanges) {
    TempDb db("commit");
    Database database(db.path);
    run(database, "CREATE TABLE t (a INTEGER);");
    run(database, "BEGIN; INSERT INTO t VALUES (1), (2), (3); COMMIT;");
    REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(3));

    run(database, "BEGIN; INSERT INTO t VALUES (4); ROLLBACK;");
    REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(3));
}

TEST(Transaction, RollbackUndoesCreateTable) {
    TempDb db("createundo");
    Database database(db.path);
    run(database, "BEGIN; CREATE TABLE gone (a INTEGER); INSERT INTO gone VALUES (1);");
    REQUIRE(database.find_table("gone") != nullptr);

    run(database, "ROLLBACK;");
    REQUIRE(database.find_table("gone") == nullptr);
    REQUIRE_EQ(database.table_count(), static_cast<std::size_t>(0));
    REQUIRE_THROWS_AS(run(database, "SELECT * FROM gone"), DbError);
}

TEST(Transaction, RollbackRewindsTheRowidCounter) {
    TempDb db("rowid");
    Database database(db.path);
    run(database, "CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1), (2);");
    run(database, "BEGIN; INSERT INTO t VALUES (3), (4); ROLLBACK;");

    // If the counter had not rewound, these would land on fresh rowids and the
    // table would still be consistent; if it rewound too far they would collide
    // with rows 1 and 2 and the count would stay at 2.
    run(database, "INSERT INTO t VALUES (30), (40);");
    REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(4));
    REQUIRE_EQ(run(database, "SELECT SUM(a) FROM t").rows[0][0], Value::integer(73));
}

TEST(Transaction, CommittedWorkSurvivesReopening) {
    TempDb db("durable");
    {
        Database database(db.path);
        run(database, "CREATE TABLE t (a INTEGER);");
        run(database, "BEGIN; INSERT INTO t VALUES (1), (2), (3); COMMIT;");
        run(database, "BEGIN; INSERT INTO t VALUES (4), (5); ROLLBACK;");
    }
    {
        Database database(db.path);
        REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(3));
        REQUIRE_EQ(run(database, "SELECT SUM(a) FROM t").rows[0][0], Value::integer(6));
    }
}

TEST(Transaction, StatementsOutsideATransactionCommitOnTheirOwn) {
    TempDb db("autocommit");
    {
        Database database(db.path);
        REQUIRE(!database.in_transaction());
        run(database, "CREATE TABLE t (a INTEGER);");
        run(database, "INSERT INTO t VALUES (7);");
        // No explicit flush, no destructor yet: autocommit already made it durable.
        REQUIRE(!database.in_transaction());
    }
    {
        Database database(db.path);
        REQUIRE_EQ(run(database, "SELECT a FROM t").rows[0][0], Value::integer(7));
    }
}

TEST(Transaction, BeginTracksStateAndRefusesToNest) {
    TempDb db("nest");
    Database database(db.path);
    run(database, "CREATE TABLE t (a INTEGER);");

    run(database, "BEGIN;");
    REQUIRE(database.in_transaction());
    REQUIRE_THROWS_AS(run(database, "BEGIN;"), DbError);

    run(database, "COMMIT;");
    REQUIRE(!database.in_transaction());
}

TEST(Transaction, ALargeTransactionSpillsToTheLogAndStillRollsBack) {
    TempDb db("spill");
    Database database(db.path);
    run(database, "CREATE TABLE t (a INTEGER, pad TEXT);");

    std::string sql = "BEGIN; INSERT INTO t VALUES ";
    for (int i = 1; i <= 3000; ++i) {  // far more than the buffer pool holds
        if (i > 1) sql += ", ";
        sql += "(" + std::to_string(i) + ", 'padding-value-" + std::to_string(i) + "')";
    }
    sql += ";";
    run(database, sql);
    REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(3000));

    run(database, "ROLLBACK;");
    REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(0));
}

TEST(Transaction, EndIsAcceptedAsASynonymForCommit) {
    TempDb db("endword");
    Database database(db.path);
    run(database, "CREATE TABLE t (a INTEGER);");
    run(database, "BEGIN TRANSACTION; INSERT INTO t VALUES (1); END;");
    REQUIRE_EQ(count(database, "t"), static_cast<std::int64_t>(1));
}
