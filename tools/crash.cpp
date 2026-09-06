// Crash-recovery torture test.
//
//   minidb_crash writer  --db P --rows N [--commit]
//       Opens the database, inserts N rows inside a transaction, optionally
//       commits, then terminates with _Exit -- no destructors, no flush, no
//       checkpoint. This is as close to `kill -9` as a program can do to itself.
//
//   minidb_crash verify  --db P --expect N
//       Reopens the database (which runs recovery) and checks that exactly the
//       committed rows are present and self-consistent.
//
//   minidb_crash torture --db P --rounds N [--seed S]
//       Repeatedly spawns itself as a writer, killing it with and without a
//       commit, and verifies after every round.
//
// The invariant: rows are inserted with id 1..n, so COUNT(*) must equal n and
// SUM(id) must equal n(n+1)/2. Any lost, duplicated or half-written row breaks
// one of the two.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "minidb/database.hpp"
#include "minidb/executor.hpp"
#include "minidb/parser.hpp"

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

std::int64_t row_count(Database& db) {
    if (db.find_table("t") == nullptr) return 0;
    return run(db, "SELECT COUNT(*) FROM t").rows[0][0].as_integer();
}

struct Options {
    std::string mode;
    std::string db = "minidb_crash.db";
    std::int64_t rows = 50;
    std::int64_t rounds = 25;
    std::int64_t expect = -1;
    std::uint64_t seed = 12345;
    bool commit = false;
};

Options parse(int argc, char** argv) {
    Options o;
    if (argc > 1) o.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--db" && i + 1 < argc) o.db = argv[++i];
        else if (arg == "--rows" && i + 1 < argc) o.rows = std::atoll(argv[++i]);
        else if (arg == "--rounds" && i + 1 < argc) o.rounds = std::atoll(argv[++i]);
        else if (arg == "--expect" && i + 1 < argc) o.expect = std::atoll(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc) o.seed = std::strtoull(argv[++i], nullptr, 10);
        else if (arg == "--commit") o.commit = true;
    }
    return o;
}

int writer(const Options& o) {
    Database db(o.db);
    run(db, "CREATE TABLE IF NOT EXISTS t (id INTEGER, pad TEXT);");

    const std::int64_t start = row_count(db) + 1;
    run(db, "BEGIN;");
    for (std::int64_t i = start; i < start + o.rows; ++i) {
        run(db, "INSERT INTO t VALUES (" + std::to_string(i) + ", 'padding-" +
                    std::to_string(i) + "');");
    }
    if (o.commit) run(db, "COMMIT;");

    std::cout.flush();
    // Die here. No destructors run, so nothing else reaches the disk.
    std::_Exit(9);
}

int verify(const Options& o, bool quiet = false) {
    Database db(o.db);
    const std::int64_t n = row_count(db);
    if (o.expect >= 0 && n != o.expect) {
        std::cerr << "verify: expected " << o.expect << " rows, found " << n << "\n";
        return 1;
    }
    if (n > 0) {
        const std::int64_t sum = run(db, "SELECT SUM(id) FROM t").rows[0][0].as_integer();
        const std::int64_t want = n * (n + 1) / 2;
        if (sum != want) {
            std::cerr << "verify: " << n << " rows but SUM(id) is " << sum << ", expected "
                      << want << "\n";
            return 1;
        }
    }
    if (!quiet) std::cout << n << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);

    if (o.mode == "writer") return writer(o);
    if (o.mode == "verify") return verify(o);

    if (o.mode != "torture") {
        std::cerr << "usage: minidb_crash writer|verify|torture --db PATH ...\n";
        return 2;
    }

    std::remove(o.db.c_str());
    std::remove((o.db + "-wal").c_str());

    const std::string self = argv[0];
    std::uint64_t state = o.seed;
    std::int64_t expected = 0;
    std::int64_t committed_rounds = 0;

    for (std::int64_t round = 1; round <= o.rounds; ++round) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const bool commit = ((state >> 33) & 1u) != 0;
        const std::int64_t rows = static_cast<std::int64_t>((state >> 20) % 120) + 1;

        std::string command = "\"" + self + "\" writer --db \"" + o.db + "\" --rows " +
                              std::to_string(rows);
        if (commit) command += " --commit";
        // Build the redirect per platform rather than appending one and
        // trimming it back -- that trim was off by five characters and
        // truncated the command on anything but Windows.
#ifdef _WIN32
        // cmd.exe strips one layer of quoting from the whole line.
        command = "\"" + command + "\" > nul 2>&1";
#else
        command += " > /dev/null 2>&1";
#endif
        std::system(command.c_str());

        if (commit) {
            expected += rows;
            ++committed_rounds;
        }

        Options check = o;
        check.expect = expected;
        if (verify(check, /*quiet=*/true) != 0) {
            std::cerr << "FAILED at round " << round << " (commit=" << commit
                      << ", rows=" << rows << ")\n";
            return 1;
        }
    }

    std::cout << "torture: " << o.rounds << " kill-and-recover rounds survived ("
              << committed_rounds << " committed, " << (o.rounds - committed_rounds)
              << " killed mid-transaction), " << expected << " rows intact\n";
    return 0;
}
