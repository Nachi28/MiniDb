// MiniDB shell: reads SQL from a file or interactively and prints result tables.
#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "minidb/database.hpp"
#include "minidb/diagnostic.hpp"
#include "minidb/executor.hpp"
#include "minidb/parser.hpp"
#include "minidb/util.hpp"

#ifdef _WIN32
#define NOMINMAX
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define MINIDB_BAD_SOCKET INVALID_SOCKET
#define MINIDB_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define MINIDB_BAD_SOCKET (-1)
#define MINIDB_CLOSE close
#endif

using namespace minidb;

namespace {

std::string trim(const std::string& s) {
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

void print_result(std::ostream& out, const ResultSet& result, double elapsed_ms) {
    if (!result.is_query) {
        out << result.message << "\n";
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
        out << "+";
        for (std::size_t w : widths) out << std::string(w + 2, '-') << "+";
        out << "\n";
    };
    const auto emit = [&](const std::vector<std::string>& line) {
        out << "|";
        for (std::size_t i = 0; i < line.size() && i < widths.size(); ++i) {
            out << " " << std::left << std::setw(static_cast<int>(widths[i])) << line[i]
                      << " |";
        }
        out << "\n";
    };

    rule();
    emit(result.column_names);
    rule();
    for (const std::vector<std::string>& line : cells) emit(line);
    rule();

    out << result.rows.size() << " row(s) in " << std::fixed << std::setprecision(3)
              << elapsed_ms << " ms\n";
}

// Returns false if a statement failed, so batch mode can exit non-zero.
bool run_sql(std::ostream& out, const std::string& source, Executor& executor) {
    try {
        Parser parser(source);
        const std::vector<Statement> statements = parser.parse_program();
        for (const Statement& stmt : statements) {
            const auto start = std::chrono::steady_clock::now();
            const ResultSet result = executor.execute(stmt);
            const auto finish = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(finish - start).count();
            print_result(out, result, ms);
        }
        return true;
    } catch (const SqlError& e) {
        out << render_error(source, e) << "\n";
        return false;
    } catch (const DbError& e) {
        out << "error: " << e.what() << "\n";
        return false;
    } catch (const std::exception& e) {
        out << "internal error: " << e.what() << "\n";
        return false;
    }
}

// ------------------------------------------------------------------ network
//
// A line protocol, not a binary one: the client sends SQL terminated by ';',
// the server replies with the same table the shell prints and a lone 0x04 to
// mark the end. Debuggable with telnet, and it reuses print_result untouched.
constexpr char kEndOfResponse = '\x04';

struct SocketSetup {
    SocketSetup() {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
    ~SocketSetup() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

bool send_all(socket_t sock, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const int n = static_cast<int>(
            send(sock, text.data() + sent, static_cast<int>(text.size() - sent), 0));
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// ponytail: one client at a time, single-threaded. Add a thread per connection
// (and a lock around Database) when concurrent clients actually matter.
int serve(Database& db, unsigned short port) {
    SocketSetup winsock;
    Executor executor(db);

    const socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == MINIDB_BAD_SOCKET) {
        std::cerr << "minidb: cannot create socket\n";
        return 2;
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 8) != 0) {
        std::cerr << "minidb: cannot listen on port " << port << "\n";
        MINIDB_CLOSE(listener);
        return 2;
    }

    std::cout << "MiniDB serving " << db.path() << " on port " << port << "\n";
    for (;;) {
        const socket_t client = accept(listener, nullptr, nullptr);
        if (client == MINIDB_BAD_SOCKET) continue;

        std::string pending;
        char buffer[4096];
        for (;;) {
            const int n = static_cast<int>(recv(client, buffer, sizeof(buffer), 0));
            if (n <= 0) break;
            pending.append(buffer, static_cast<std::size_t>(n));

            // Statements are dispatched once the buffer ends in a semicolon, the
            // same rule the interactive shell uses.
            std::string trimmed = trim(pending);
            if (trimmed.empty() || trimmed.back() != ';') continue;

            std::ostringstream reply;
            run_sql(reply, pending, executor);
            reply << kEndOfResponse << "\n";
            pending.clear();
            if (!send_all(client, reply.str())) break;
        }
        MINIDB_CLOSE(client);
        db.flush();
    }
}

int run_client(const std::string& host, unsigned short port) {
    SocketSetup winsock;

    const socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == MINIDB_BAD_SOCKET) {
        std::cerr << "minidb: cannot create socket\n";
        return 2;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        std::cerr << "minidb: bad host " << host << "\n";
        return 2;
    }
    if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "minidb: cannot connect to " << host << ":" << port << "\n";
        MINIDB_CLOSE(sock);
        return 2;
    }
    std::cout << "connected to " << host << ":" << port << "\n";

    std::string pending;
    std::string line;
    while (std::cout << (pending.empty() ? "minidb> " : "   ...> ") << std::flush,
           std::getline(std::cin, line)) {
        const std::string command = trim(line);
        if (command == ".exit" || command == ".quit") break;

        pending += line;
        pending += "\n";
        if (trim(pending).empty() || trim(pending).back() != ';') continue;

        if (!send_all(sock, pending)) break;
        pending.clear();

        char buffer[4096];
        bool done = false;
        while (!done) {
            const int n = static_cast<int>(recv(sock, buffer, sizeof(buffer), 0));
            if (n <= 0) {
                done = true;
                break;
            }
            for (int i = 0; i < n; ++i) {
                if (buffer[i] == kEndOfResponse) {
                    done = true;
                    break;
                }
                std::cout << buffer[i];
            }
        }
    }
    MINIDB_CLOSE(sock);
    return 0;
}

void print_help() {
    std::cout << "MiniDB shell commands:\n"
              << "  .help              show this message\n"
              << "  .tables            list tables\n"
              << "  .indexes           list indexes\n"
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
    } else if (command == ".indexes") {
        const std::vector<std::string> names = db.index_names();
        if (names.empty()) {
            std::cout << "(no indexes)\n";
        } else {
            for (const std::string& name : names) {
                const Index* index = db.find_index(name);
                if (index == nullptr) continue;
                std::cout << name << " on " << index->table() << "("
                          << index->column() << ")\n";
            }
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
    std::string connect_to;
    unsigned short port = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--serve" && i + 1 < argc) {
            port = static_cast<unsigned short>(std::atoi(argv[++i]));
        } else if (arg == "--connect" && i + 1 < argc) {
            connect_to = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: minidb [--db FILE] [SCRIPT.sql]\n"
                      << "       minidb --db FILE --serve PORT\n"
                      << "       minidb --connect HOST:PORT\n";
            return 0;
        } else if (script.empty()) {
            script = arg;
        } else {
            std::cerr << "minidb: unexpected argument " << arg << "\n";
            return 2;
        }
    }

    if (!connect_to.empty()) {
        const std::size_t colon = connect_to.rfind(':');
        if (colon == std::string::npos) {
            std::cerr << "minidb: --connect expects HOST:PORT\n";
            return 2;
        }
        return run_client(connect_to.substr(0, colon),
                          static_cast<unsigned short>(std::atoi(
                              connect_to.substr(colon + 1).c_str())));
    }

    // Opening the database can fail -- a missing directory, a file that is not
    // a MiniDB database. Report it rather than terminating on an uncaught
    // exception with no message at all.
    std::unique_ptr<Database> owned;
    try {
        owned = std::make_unique<Database>(db_path);
    } catch (const std::exception& e) {
        std::cerr << "minidb: " << e.what() << "\n";
        return 2;
    }
    Database& db = *owned;

    if (port != 0) return serve(db, port);

    Executor executor(db);

    if (!script.empty()) {  // batch mode
        std::ifstream file(script);
        if (!file) {
            std::cerr << "minidb: cannot open " << script << "\n";
            return 2;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const bool ok = run_sql(std::cout, buffer.str(), executor);
        db.flush();
        return ok ? 0 : 1;
    }

    std::cout << "MiniDB 1.0.0 - phase 6 (server)\n"
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
            run_sql(std::cout, pending, executor);
            pending.clear();
        }
    }

    if (!trim(pending).empty()) run_sql(std::cout, pending, executor);
    return 0;
}
