// MiniDB benchmark harness.
//   minidb_bench [--rows N] [--db PATH] [--pool PAGES]
//
// Reports storage-level and SQL-level throughput plus buffer-pool behaviour, so
// a change can be judged against the numbers it moved rather than by feel.
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "minidb/database.hpp"
#include "minidb/executor.hpp"
#include "minidb/key.hpp"
#include "minidb/parser.hpp"

using namespace minidb;

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void report(const std::string& what, std::int64_t count, double seconds,
            const std::string& unit) {
    const double rate = seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0;
    std::cout << "  " << std::left << std::setw(26) << what << std::right << std::setw(10)
              << count << " " << std::setw(6) << unit << "  " << std::setw(8) << std::fixed
              << std::setprecision(3) << seconds << " s  " << std::setw(12)
              << static_cast<std::int64_t>(rate) << " " << unit << "/s\n";
}

struct Lcg {
    std::uint64_t state = 88172645463325252ULL;
    std::uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 16;
    }
};

ResultSet run_sql(Database& db, const std::string& sql) {
    Parser parser(sql);
    Executor executor(db);
    ResultSet last;
    const std::vector<Statement> statements = parser.parse_program();
    for (const Statement& stmt : statements) last = executor.execute(stmt);
    return last;
}

}  // namespace

int main(int argc, char** argv) {
    std::int64_t rows = 200000;
    std::string path = "minidb_bench.db";
    std::size_t pool = 256;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rows" && i + 1 < argc) rows = std::atoll(argv[++i]);
        else if (arg == "--db" && i + 1 < argc) path = argv[++i];
        else if (arg == "--pool" && i + 1 < argc) pool = static_cast<std::size_t>(std::atoll(argv[++i]));
        else {
            std::cerr << "usage: minidb_bench [--rows N] [--db PATH] [--pool PAGES]\n";
            return 2;
        }
    }

    std::remove(path.c_str());
    std::cout << "MiniDB benchmark: " << rows << " rows, " << pool << "-page pool, "
              << (path == Database::kMemory ? "in memory" : path) << "\n\n";

    Database db(path);
    run_sql(db, "CREATE TABLE bench (id INTEGER, name TEXT, score REAL);");
    Table* table = db.find_table("bench");

    // 1. Insert through the storage layer.
    {
        const Clock::time_point start = Clock::now();
        for (std::int64_t i = 1; i <= rows; ++i) {
            Row row = {Value::integer(i), Value::text("name-" + std::to_string(i)),
                       Value::real(static_cast<double>(i) * 0.5)};
            table->insert(std::move(row));
        }
        report("insert (storage)", rows, seconds_since(start), "rows");
    }

    // 2. Durable write.
    {
        const Clock::time_point start = Clock::now();
        db.flush();
        report("flush to disk", 1, seconds_since(start), "calls");
    }

    // 3. Full scan through the storage layer.
    {
        const Clock::time_point start = Clock::now();
        std::int64_t seen = 0;
        table->scan([&](std::int64_t, Row&) {
            ++seen;
            return true;
        });
        report("full scan (storage)", seen, seconds_since(start), "rows");
    }

    // 4. Random point lookups.
    {
        const std::int64_t probes = rows / 10 + 1;
        Lcg rng;
        const Clock::time_point start = Clock::now();
        std::int64_t found = 0;
        std::vector<std::uint8_t> payload;
        BTree tree(db.pager(), table->root());
        for (std::int64_t i = 0; i < probes; ++i) {
            const std::int64_t key =
                static_cast<std::int64_t>(rng.next() % static_cast<std::uint64_t>(rows)) + 1;
            const std::string encoded = encode_rowid(key);
            if (tree.find(Slice(encoded), &payload)) ++found;
        }
        report("point lookup (B+tree)", found, seconds_since(start), "gets");
    }

    // 5. SQL aggregate over the whole table.
    {
        const Clock::time_point start = Clock::now();
        const ResultSet r = run_sql(db, "SELECT COUNT(*), SUM(id), AVG(score) FROM bench");
        const double elapsed = seconds_since(start);
        report("SELECT aggregate (SQL)", rows, elapsed, "rows");
        std::cout << "    -> " << r.rows[0][0].to_string() << " rows, sum "
                  << r.rows[0][1].to_string() << "\n";
    }

    // 6. SQL filtered scan.
    {
        const Clock::time_point start = Clock::now();
        const ResultSet r = run_sql(db, "SELECT COUNT(*) FROM bench WHERE score > 1000.0");
        report("SELECT with WHERE (SQL)", rows, seconds_since(start), "rows");
        std::cout << "    -> " << r.rows[0][0].to_string() << " matched\n";
    }

    // 7. The same selective query with and without an index on the column.
    {
        const std::string selective = "SELECT COUNT(*) FROM bench WHERE id = " +
                                      std::to_string(rows / 2) + ";";
        Clock::time_point start = Clock::now();
        run_sql(db, selective);
        const double without = seconds_since(start);

        run_sql(db, "CREATE INDEX idx_id ON bench (id);");
        start = Clock::now();
        run_sql(db, selective);
        const double with = seconds_since(start);

        std::cout << "  selective query" << "\n"
                  << "    without index      " << std::fixed << std::setprecision(4)
                  << without * 1000.0 << " ms" << "\n"
                  << "    with index         " << with * 1000.0 << " ms"
                  << "   (" << std::setprecision(1)
                  << (with > 0.0 ? without / with : 0.0) << "x faster)" << "\n";
    }

    std::cout << "\n  pages allocated      " << db.pager().page_count() << "\n"
              << "  cache hits/misses    " << db.pager().cache_hits() << " / "
              << db.pager().cache_misses() << "\n"
              << "  disk reads/writes    " << db.pager().disk_reads() << " / "
              << db.pager().disk_writes() << "\n"
              << "  evictions            " << db.pager().evictions() << "\n";

    std::cout << "\n  hit rate             " << std::fixed << std::setprecision(1)
              << (100.0 * static_cast<double>(db.pager().cache_hits()) /
                  static_cast<double>(db.pager().cache_hits() + db.pager().cache_misses()))
              << " %\n";
    return 0;
}
