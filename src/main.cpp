// MiniDB shell: reads SQL from a file or interactively and prints result tables.
#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "minidb/database.hpp"
#include "minidb/diagnostic.hpp"
#include "minidb/executor.hpp"
#include "minidb/parser.hpp"
#include "minidb/util.hpp"

using namespace minidb;

namespace {

std::string trim(const std::string& s) {
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

void print_result(const ResultSet& result, double elapsed_ms) {
    if (!result.is_query) {
        std::cout << result.message << "\n";
        return;
    }

    std::vector<std::size_t> widths;
    widths.reserve(result.column_names.size());
    for (const std::string& name : result.column_names) widths.push_back(name.size());

    std::vector<std::vector<std::string>> cells;
    cells.reserve(result.rows.size());
    for (const Row& row : result.rows) {
        std::vector<std::string> line;
        line.reserve(row.size());
        for (std::size_t i = 0; i < row.size(); ++i) {
            std::string text = row[i].to_string();
            if (i < widths.size()) widths[i] = std::max(widths[i], text.size());
            line.push_back(std::move(text));
        }
        cells.push_back(std::move(line));
    }

    const auto rule = [&]() {
        std::cout << "+";
        for (std::size_t w : widths) std::cout << std::string(w + 2, '-') << "+";
        std::cout << "\n";
    };
    const auto emit = [&](const std::vector<std::string>& line) {
        std::cout << "|";
        for (std::size_t i = 0; i < line.size() && i < widths.size(); ++i) {
            std::cout << " " << std::left << std::setw(static_cast<int>(widths[i])) << line[i]
                      << " |";
        }
        std::cout << "\n";
    };

    rule();
    emit(result.column_names);
    rule();
    for (const std::vector<std::string>& line : cells) emit(line);
    rule();

    std::cout << result.rows.size() << " row(s) in " << std::fixed << std::setprecision(3)
              << elapsed_ms << " ms\n";
}

// Returns false if a statement failed, so batch mode can exit non-zero.
bool run_sql(const std::string& source, Executor& executor) {
    try {
        Parser parser(source);
        const std::vector<Statement> statements = parser.parse_program();
        for (const Statement& stmt : statements) {
            const auto start = std::chrono::steady_clock::now();
            const ResultSet result = executor.execute(stmt);
            const auto finish = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(finish - start).count();
            print_result(result, ms);
        }
        return true;
    } catch (const SqlError& e) {
        std::cout << render_error(source, e) << "\n";
        return false;
    } catch (const DbError& e) {
        std::cout << "error: " << e.what() << "\n";
        return false;
    } catch (const std::exception& e) {
        std::cout << "internal error: " << e.what() << "\n";
        return false;
    }
}

void print_help() {
    std::cout << "MiniDB shell commands:\n"
              << "  .help              show this message\n"
              << "  .tables            list tables\n"
              << "  .schema [table]    show column definitions\n"
              << "  .flush             write everything to disk now\n"
              << "  .exit / .quit      leave the shell\n"
              << "Anything else is SQL; statements end with ';'.\n";
}

void print_schema(const Database& db, const std::string& only) {
    for (const std::string& name : db.table_names()) {
        if (!only.empty() && !iequals(name, only)) continue;
        const Table* table = db.find_table(name);
        if (table == nullptr) continue;
        std::cout << "CREATE TABLE " << table->name() << " (\n";
        for (std::size_t i = 0; i < table->columns().size(); ++i) {
            const ColumnDef& column = table->columns()[i];
            std::cout << "    " << column.name << " " << type_name(column.type)
                      << (i + 1 < table->columns().size() ? "," : "") << "\n";
        }
        std::cout << ");\n";
    }
}

// Returns true if the line was a shell command (and was handled).
bool handle_dot_command(const std::string& line, Database& db, bool& should_exit) {
    if (line.empty() || line[0] != '.') return false;

    std::istringstream in(line);
    std::string command;
    std::string argument;
    in >> command >> argument;

    if (command == ".exit" || command == ".quit") {
        should_exit = true;
    } else if (command == ".help") {
        print_help();
    } else if (command == ".tables") {
        const std::vector<std::string> names = db.table_names();
        if (names.empty()) {
            std::cout << "(no tables)\n";
        } else {
            for (const std::string& name : names) std::cout << name << "\n";
        }
    } else if (command == ".schema") {
        print_schema(db, argument);
    } else if (command == ".flush") {
        db.flush();
        std::cout << "flushed" << "\n";
    } else {
        std::cout << "unknown command: " << command << " (try .help)\n";
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string db_path = Database::kMemory;
    std::string script;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: minidb [--db FILE] [SCRIPT.sql]\n";
            return 0;
        } else if (script.empty()) {
            script = arg;
        } else {
            std::cerr << "minidb: unexpected argument " << arg << "\n";
            return 2;
        }
    }

    Database db(db_path);
    Executor executor(db);

    if (!script.empty()) {  // batch mode
        std::ifstream file(script);
        if (!file) {
            std::cerr << "minidb: cannot open " << script << "\n";
            return 2;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const bool ok = run_sql(buffer.str(), executor);
        db.flush();
        return ok ? 0 : 1;
    }

    std::cout << "MiniDB 0.4.0 - phase 4 (write-ahead log)\n"
              << "database: " << db.path() << "\n"
              << "Type .help for shell commands, .exit to quit.\n";

    std::string pending;
    std::string line;
    bool should_exit = false;
    while (!should_exit) {
        std::cout << (pending.empty() ? "minidb> " : "   ...> ") << std::flush;
        if (!std::getline(std::cin, line)) break;

        if (pending.empty() && handle_dot_command(trim(line), db, should_exit)) continue;

        pending += line;
        pending += "\n";

        // Statements are dispatched once the buffer ends in a semicolon.
        // (A ';' inside a string literal would fool this; the parser, not the
        // shell, is the real authority, so it only affects interactive editing.)
        const std::string trimmed = trim(pending);
        if (!trimmed.empty() && trimmed.back() == ';') {
            run_sql(pending, executor);
            pending.clear();
        }
    }

    if (!trim(pending).empty()) run_sql(pending, executor);
    return 0;
}
