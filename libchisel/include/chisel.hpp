//
// Created by Giuseppe Francione on 09/12/25.
//

/**
 * @file chisel.hpp
 * @brief Public API for the Chisel library.
 */

#ifndef CHISEL_HPP
#define CHISEL_HPP

#include <string>
#include <vector>
#include <filesystem>
#include <memory>
#include <cstdint>

namespace chisel {

/**
 * @brief Defines the strategy for applying multiple processors to a single file.
 */
enum class EncodeMode {
    /**
     * @brief Chain processors: output of one is input to the next.
     */
    PIPE,
    /**
     * @brief Run all applicable processors on the original file independently.
     */
    PARALLEL
};

/**
 * @brief Interface for receiving progress and status events during execution.
 */
struct ChiselObserver {
    virtual ~ChiselObserver() = default;

    virtual void onFileStart(const std::filesystem::path& path) {}

    virtual void onFileFinish(const std::filesystem::path& path,
                              uintmax_t size_before,
                              uintmax_t size_after,
                              bool replaced) {}

    virtual void onFileError(const std::filesystem::path& path,
                             const std::string& error) {}

    virtual void onLog(int level, const std::string& msg, const std::string& tag) {}
};

/**
 * @brief Main interface for the Chisel library.
 *
 * @details Wraps the optimization pipeline into a simple, blocking API.
 * Uses PIMPL idiom to hide internal dependencies.
 */
class Chisel {
public:
    Chisel();
    ~Chisel();

    Chisel(const Chisel&) = delete;
    Chisel& operator=(const Chisel&) = delete;
    Chisel(Chisel&&) noexcept;
    Chisel& operator=(Chisel&&) noexcept;

    // --- Configuration ---

    /**
     * @brief Enable or disable metadata preservation.
     * Default: true.
     */
    Chisel& preserveMetadata(bool val);

    /**
     * @brief Enable or disable raw checksum verification.
     * Default: false.
     */
    Chisel& verifyChecksums(bool val);

    /**
     * @brief Enable or disable dry-run mode.
     * Default: false.
     */
    Chisel& dryRun(bool val);

    /**
     * @brief Set the number of worker threads to use.
     * Default: hardware concurrency / 2.
     */
    Chisel& threads(unsigned val);

    /**
     * @brief Set the encoding strategy.
     * Default: PIPE.
     */
    Chisel& mode(EncodeMode m);

    /**
     * @brief Set a separate output directory.
     * Default: empty (in-place).
     */
    Chisel& outputDirectory(const std::filesystem::path& dir);

    // --- Observability ---

    /**
     * @brief Sets the observer for progress events.
     * The caller retains ownership of the observer.
     */
    void setObserver(ChiselObserver* observer);

    // --- Execution ---

    /**
     * @brief Recompresses a list of files. Blocks until completion.
     */
    void recompress(const std::vector<std::filesystem::path>& paths);

    void recompress(const std::filesystem::path& path);
    void recompress(const std::vector<std::string>& paths);

    // --- Control ---

    /**
     * @brief Requests cancellation. Thread-safe.
     */
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chisel

#endif // CHISEL_HPP