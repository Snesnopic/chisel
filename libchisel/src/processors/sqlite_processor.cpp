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


namespace chisel {

void SqliteProcessor::recompress(const std::filesystem::path& input,
                                 const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    // open the ORIGINAL (not a filesystem copy) so a live WAL journal is
    // transparently merged in by the normal SQLite read path; a plain
    // copy_file() of just the main .db file would silently drop any
    // committed data still sitting in an uncheckpointed -wal file
    sqlite3* src_db = nullptr;
    int rc = sqlite3_open_v2(input.string().c_str(), &src_db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        const std::string err_msg = src_db ? sqlite3_errmsg(src_db) : "unknown error";
        if (src_db) sqlite3_close(src_db);
        Logger::log(LogLevel::Warning, "Cannot open database (likely corrupt): " + err_msg, get_name());
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
        return; // act as passthrough
    }

    // VACUUM INTO reads through the same WAL-aware path as any normal query,
    // so it produces a complete, already-vacuumed copy in one step
    std::error_code rm_ec;
    std::filesystem::remove(output, rm_ec); // VACUUM INTO refuses to overwrite an existing file
    char* vacuum_sql = sqlite3_mprintf("VACUUM INTO %Q;", output.string().c_str());
    rc = sqlite3_exec(src_db, vacuum_sql, nullptr, nullptr, nullptr);
    sqlite3_free(vacuum_sql);
    if (rc != SQLITE_OK) {
        const std::string err_msg = sqlite3_errmsg(src_db);
        sqlite3_close(src_db);
        Logger::log(LogLevel::Warning, "Vacuum failed (likely corrupt): " + err_msg, get_name());
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
        return; // act as passthrough
    }
    sqlite3_close(src_db);
    Logger::log(LogLevel::Debug, "Vacuum completed", get_name());

    // run ANALYZE on the new output
    sqlite3* db = nullptr;
    rc = sqlite3_open_v2(output.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
        const std::string err_msg = db ? sqlite3_errmsg(db) : "unknown error";
        if (db) sqlite3_close(db);
        Logger::log(LogLevel::Warning, "Cannot open vacuumed database: " + err_msg, get_name());
        return; // act as passthrough (vacuumed file already written)
    }

    rc = sqlite3_exec(db, "ANALYZE;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        const std::string err_msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        Logger::log(LogLevel::Warning, "Analyze failed (likely corrupt): " + err_msg, get_name());
        return; // act as passthrough (vacuumed file already written, just not analyzed)
    }
    Logger::log(LogLevel::Debug, "Analyze completed", get_name());

    sqlite3_close(db);

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

// dumps every row of one table into out_rows, one string per row (column
// values joined by '|'); BLOBs are hex-encoded (sqlite3_exec's text-only
// callback interface would silently truncate them at the first embedded
// NUL byte instead)
static void dump_table_rows(sqlite3* db, const std::string& table, std::vector<std::string>& out_rows) {
    const std::string sql = "SELECT * FROM \"" + table + "\";";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return;

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
    int rc = sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        Logger::log(LogLevel::Warning, "Raw_equal: Cannot open database: " + file.string(), "SqliteProcessor");
        return ""; // return empty on error
    }

    std::stringstream dump_stream;
    char *err_msg = nullptr;

    // integrity check, plus schema dump excluding internal sqlite_* objects
    // (ANALYZE creates sqlite_stat1/sqlite_stat4, which would otherwise make
    // every correctly-recompressed database look "different" from its source)
    rc = sqlite3_exec(db,
        "PRAGMA integrity_check; "
        "SELECT type, name, tbl_name, sql FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY name;",
        sqlite_dump_callback, &dump_stream, &err_msg);

    if (rc != SQLITE_OK) {
        Logger::log(LogLevel::Warning, "Raw_equal: Failed to dump database: " + std::string(err_msg),
                    "SqliteProcessor");
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return ""; // return empty on error
    }

    // actual row data per user table, sorted per-table for order-independence
    // (VACUUM can physically reorder rows without changing their content)
    std::vector<std::string> table_names;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY name;",
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