//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/sqlite_processor.hpp"
#include "../../include/logger.hpp"
#include <sqlite3.h>
#include <stdexcept>
#include <filesystem>

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

std::string SqliteProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw SQLite file
    return "";
}

} // namespace chisel