//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/sqlite_processor.hpp"
#include "../../include/logger.hpp"
#include <sqlite3.h>
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <string>

namespace chisel {

void SqliteProcessor::recompress(const std::filesystem::path& input,
                                 const std::filesystem::path& output,
                                 bool /*preserve_metadata*/) {
    Logger::log(LogLevel::Info, "Starting SQLite recompression: " + input.string(), "sqlite_processor");

    // copy input to output
    try {
        std::filesystem::copy_file(input, output,
                                   std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Failed to copy input to output: " + std::string(e.what()), "sqlite_processor");
        throw;
    }

    // open output database
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(output.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        Logger::log(LogLevel::Error, "Cannot open database: " + std::string(sqlite3_errmsg(db)), "sqlite_processor");
        throw std::runtime_error("SqliteProcessor: cannot open database");
    }

    // run VACUUM
    rc = sqlite3_exec(db, "VACUUM;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        Logger::log(LogLevel::Error, "VACUUM failed: " + std::string(sqlite3_errmsg(db)), "sqlite_processor");
        sqlite3_close(db);
        throw std::runtime_error("SqliteProcessor: VACUUM failed");
    }
    Logger::log(LogLevel::Info, "VACUUM completed", "sqlite_processor");

    // run ANALYZE
    rc = sqlite3_exec(db, "ANALYZE;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        Logger::log(LogLevel::Error, "ANALYZE failed: " + std::string(sqlite3_errmsg(db)), "sqlite_processor");
        sqlite3_close(db);
        throw std::runtime_error("SqliteProcessor: ANALYZE failed");
    }
    Logger::log(LogLevel::Info, "ANALYZE completed", "sqlite_processor");

    sqlite3_close(db);

    Logger::log(LogLevel::Info, "SQLite recompression completed: " + output.string(), "sqlite_processor");
}

// helper callback for sqlite3_exec to accumulate dump output
static int sqlite_dump_callback(void *user_data, int argc, char **argv, char **azColName) {
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
        Logger::log(LogLevel::Warning, "raw_equal: Cannot open database: " + file.string(), "sqlite_processor");
        return ""; // return empty on error
    }

    std::stringstream dump_stream;
    char *err_msg = nullptr;

    // dump schema and data
    rc = sqlite3_exec(db, ".dump", sqlite_dump_callback, &dump_stream, &err_msg);

    if (rc != SQLITE_OK) {
        Logger::log(LogLevel::Warning, "raw_equal: Failed to dump database: " + std::string(err_msg),
                    "sqlite_processor");
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
        Logger::log(LogLevel::Error, "raw_equal: Error dumping " + a.string() + ": " + e.what(), "sqlite_processor");
        return false;
    }

    try {
        dumpB = dump_sqlite_db(b);
    } catch (const std::exception &e) {
        Logger::log(LogLevel::Error, "raw_equal: Error dumping " + b.string() + ": " + e.what(), "sqlite_processor");
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