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


namespace chisel {

void SqliteProcessor::recompress(const std::filesystem::path& input,
                                 const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    // copy input to output
    try {
        std::filesystem::copy_file(input, output,
                                   std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Failed to copy input to output: " + std::string(e.what()), get_name());
        throw;
    }

    // open output database
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(output.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
        const std::string err_msg = db ? sqlite3_errmsg(db) : "unknown error";
        if (db) sqlite3_close(db);
        Logger::log(LogLevel::Warning, "Cannot open database (likely corrupt): " + err_msg, get_name());
        return; // act as passthrough (file already copied)
    }

    // run VACUUM
    rc = sqlite3_exec(db, "VACUUM;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        const std::string err_msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        Logger::log(LogLevel::Warning, "Vacuum failed (likely corrupt): " + err_msg, get_name());
        return; // act as passthrough (file already copied, though partially vacuumed, size won't improve)
    }
    Logger::log(LogLevel::Debug, "Vacuum completed", get_name());

    // run ANALYZE
    rc = sqlite3_exec(db, "ANALYZE;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        const std::string err_msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        Logger::log(LogLevel::Warning, "Analyze failed (likely corrupt): " + err_msg, get_name());
        return; // act as passthrough
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

// helper function to dump a database to a string
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

    // dump schema and verify integrity instead of using CLI .dump
    rc = sqlite3_exec(db, "PRAGMA integrity_check; SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY name;", sqlite_dump_callback, &dump_stream, &err_msg);

    if (rc != SQLITE_OK) {
        Logger::log(LogLevel::Warning, "Raw_equal: Failed to dump database: " + std::string(err_msg),
                    "SqliteProcessor");
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return ""; // return empty on error
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