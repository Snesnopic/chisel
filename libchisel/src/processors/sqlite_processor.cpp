//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/sqlite_processor.hpp"
#include "../../include/logger.hpp"
#include "sqlite3.h"
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>


namespace chisel {

namespace {

// sqlite takes file names as utf-8
std::string utf8(const std::filesystem::path& file) {
    const std::u8string name = file.u8string();
    return {name.begin(), name.end()};
}

// immutable: sqlite takes no locks and creates no -wal or -shm next to the file
std::string immutable_uri(const std::filesystem::path& file) {
    std::error_code ec;
    const std::u8string name = std::filesystem::absolute(file, ec).generic_u8string();
    std::string uri = "file://";
    if (name.empty() || name.front() != u8'/') uri += '/'; // file:///C:/...
    static const char hex[] = "0123456789ABCDEF";
    for (const char8_t c : name) {
        const auto b = static_cast<unsigned char>(c);
        if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || std::strchr("-._~/:", b)) {
            uri += static_cast<char>(b);
        } else {
            uri += '%';
            uri += hex[b >> 4];
            uri += hex[b & 0xF];
        }
    }
    return uri + "?immutable=1";
}

bool has_sidecar(const std::filesystem::path& file, std::string* found = nullptr) {
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        std::filesystem::path sidecar = file;
        sidecar += suffix;
        std::error_code ec;
        if (std::filesystem::exists(sidecar, ec)) {
            if (found) *found = suffix;
            return true;
        }
    }
    return false;
}

std::optional<std::pair<std::uintmax_t, std::filesystem::file_time_type>> stamp_of(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) return std::nullopt;
    const auto mtime = std::filesystem::last_write_time(file, ec);
    if (ec) return std::nullopt;
    return std::pair{size, mtime};
}

} // namespace

void SqliteProcessor::recompress(const std::filesystem::path& input,
                                 const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    const auto passthrough = [&](const std::string& reason) {
        Logger::log(LogLevel::Warning, reason, get_name());
        std::error_code ec;
        std::filesystem::remove(output, ec);
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
    };

    // an app still has it open, or didn't close it: its -wal or -journal would be replayed onto the rewritten file
    if (std::string suffix; has_sidecar(input, &suffix)) {
        Logger::log(LogLevel::Info, "Skipping " + input.filename().string() + ": in use, with a " +
                    suffix + " file next to it", get_name());
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
        return;
    }

    const auto stamp = stamp_of(input);

    // with no -wal or -journal next to it, the main file holds every committed change
    sqlite3* src_db = nullptr;
    int rc = sqlite3_open_v2(immutable_uri(input).c_str(), &src_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK) {
        const std::string err_msg = src_db ? sqlite3_errmsg(src_db) : "unknown error";
        if (src_db) sqlite3_close(src_db);
        passthrough("Cannot open database (likely corrupt): " + err_msg);
        return;
    }

    std::error_code rm_ec;
    std::filesystem::remove(output, rm_ec); // VACUUM INTO refuses to overwrite an existing file
    char* vacuum_sql = sqlite3_mprintf("VACUUM INTO %Q;", utf8(output).c_str());
    rc = sqlite3_exec(src_db, vacuum_sql, nullptr, nullptr, nullptr);
    sqlite3_free(vacuum_sql);
    if (rc != SQLITE_OK) {
        const std::string err_msg = sqlite3_errmsg(src_db);
        sqlite3_close(src_db);
        passthrough("Vacuum failed (likely corrupt): " + err_msg);
        return;
    }
    sqlite3_close(src_db);
    Logger::log(LogLevel::Debug, "Vacuum completed", get_name());

    // immutable reads take no locks, so a write in the meantime would go unnoticed
    if (!stamp || stamp_of(input) != stamp || has_sidecar(input)) {
        passthrough("Changed while being read, left as is: " + input.filename().string());
        return;
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

// helper callback for sqlite3_exec to accumulate dump output
static int sqlite_dump_callback(void *user_data, const int argc, char **argv, char **azColName) {
    auto *out_stream = static_cast<std::stringstream *>(user_data);
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            *out_stream << azColName[i] << " = " << argv[i] << "\n";
        }
    }
    return 0;
}

// dumps every row of one table into out_rows, one string per row (rowid and
// column values joined by '|'); BLOBs are hex-encoded (sqlite3_exec's text-only
// callback interface would silently truncate them at the first embedded
// NUL byte instead)
static void dump_table_rows(sqlite3* db, const std::string& table, std::vector<std::string>& out_rows) {
    sqlite3_stmt* stmt = nullptr;
    // a WITHOUT ROWID table has no rowid to select
    for (const char* columns : {"rowid, *", "*"}) {
        char* sql = sqlite3_mprintf("SELECT %s FROM \"%w\";", columns, table.c_str());
        const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        sqlite3_free(sql);
        if (rc == SQLITE_OK) break;
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
    if (!stmt) return;

    const int ncols = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string row;
        for (int i = 0; i < ncols; i++) {
            if (i) row += "|";
            if (sqlite3_column_type(stmt, i) == SQLITE_BLOB) {
                const auto* data = static_cast<const unsigned char*>(sqlite3_column_blob(stmt, i));
                const int n = sqlite3_column_bytes(stmt, i);
                static const char hex[] = "0123456789abcdef";
                for (int b = 0; b < n; b++) {
                    row += hex[data[b] >> 4];
                    row += hex[data[b] & 0xF];
                }
            } else {
                const auto* text = sqlite3_column_text(stmt, i);
                row += text ? reinterpret_cast<const char*>(text) : "<NULL>";
            }
        }
        out_rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
}

// helper function to dump a database (schema + actual row data) to a string
std::string dump_sqlite_db(const std::filesystem::path &file) {
    sqlite3 *db = nullptr;
    int rc = sqlite3_open_v2(immutable_uri(file).c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        Logger::log(LogLevel::Warning, "Raw_equal: Cannot open database: " + file.string(), "SqliteProcessor");
        return ""; // return empty on error
    }

    std::stringstream dump_stream;
    char *err_msg = nullptr;

    // integrity check, plus schema dump (sqlite_sequence and sqlite_stat* included)
    rc = sqlite3_exec(db,
        "PRAGMA integrity_check; "
        "SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY name;",
        sqlite_dump_callback, &dump_stream, &err_msg);

    if (rc != SQLITE_OK) {
        Logger::log(LogLevel::Warning, "Raw_equal: Failed to dump database: " + std::string(err_msg),
                    "SqliteProcessor");
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return ""; // return empty on error
    }

    // actual row data per table, sorted per-table for order-independence
    // (VACUUM can physically reorder rows without changing their content)
    std::vector<std::string> table_names;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;",
            -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto* name = sqlite3_column_text(stmt, 0);
            if (name) table_names.emplace_back(reinterpret_cast<const char*>(name));
        }
    }
    sqlite3_finalize(stmt);

    for (const auto& table : table_names) {
        std::vector<std::string> rows;
        dump_table_rows(db, table, rows);
        std::sort(rows.begin(), rows.end());
        for (const auto& row : rows) {
            dump_stream << table << ":" << row << "\n";
        }
    }

    sqlite3_close(db);
    return dump_stream.str();
}


[[nodiscard]] bool SqliteProcessor::raw_equal(const std::filesystem::path &a,
                                              const std::filesystem::path &b) const {
    std::string dumpA, dumpB;
    try {
        dumpA = dump_sqlite_db(a);
    } catch (const std::exception &e) {
        Logger::log(LogLevel::Error, "Raw_equal: Error dumping " + a.string() + ": " + e.what(), get_name());
        return false;
    }

    try {
        dumpB = dump_sqlite_db(b);
    } catch (const std::exception &e) {
        Logger::log(LogLevel::Error, "Raw_equal: Error dumping " + b.string() + ": " + e.what(), get_name());
        return false;
    }

    if (dumpA.empty() || dumpB.empty()) {
        return false; // dump failed for one or both
    }

    return dumpA == dumpB;
}
std::string SqliteProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw SQLite file
    return "";
}

} // namespace chisel