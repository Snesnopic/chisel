//
// Created by Giuseppe Francione on 01/10/25.
//

#include "sqlite_encoder.hpp"
#include "../utils/logger.hpp"
#include <sqlite3.h>
#include <stdexcept>
#include <filesystem>

SqliteEncoder::SqliteEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

// recompress a sqlite database by copying input to output and running vacuum/analyze
bool SqliteEncoder::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output) {
    Logger::log(LogLevel::Info, "Starting SQLite recompression: " + input.string(), "sqlite_encoder");

    // copy input to output
    try {
        std::filesystem::copy_file(input, output,
                                   std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Failed to copy input to output: " + std::string(e.what()), "sqlite_encoder");
        throw std::runtime_error("Failed to copy input to output");
    }

    // open output database
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(output.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
        Logger::log(LogLevel::Error, "Cannot open database: " + std::string(sqlite3_errmsg(db)), "sqlite_encoder");
        if (db) sqlite3_close(db);
        throw std::runtime_error("Cannot open database");
    }

    // run vacuum
    rc = sqlite3_exec(db, "VACUUM;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        Logger::log(LogLevel::Error, "VACUUM failed: " + std::string(sqlite3_errmsg(db)), "sqlite_encoder");
        sqlite3_close(db);
        throw std::runtime_error("VACUUM failed");
    }
    Logger::log(LogLevel::Info, "VACUUM completed", "sqlite_encoder");

    // run analyze
    rc = sqlite3_exec(db, "ANALYZE;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        Logger::log(LogLevel::Error, "ANALYZE failed: " + std::string(sqlite3_errmsg(db)), "sqlite_encoder");
        sqlite3_close(db);
        throw std::runtime_error("ANALYZE failed");
    }
    Logger::log(LogLevel::Info, "ANALYZE completed", "sqlite_encoder");

    sqlite3_close(db);

    Logger::log(LogLevel::Info, "SQLite recompression completed: " + output.string(), "sqlite_encoder");
    return true;
}