#ifndef CHISEL_APP_STATS_HPP
#define CHISEL_APP_STATS_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chisel_app {

    // one queued file that got smaller and was saved
    struct StatRecord {
        std::int64_t timestamp = 0; // seconds since the unix epoch, utc
        std::string extension;      // lowercase, no dot
        std::uint64_t original_size = 0;
        std::uint64_t compressed_size = 0;
        std::uint64_t duration_ms = 0;
        std::string run_id;
        std::string file_name;      // utf-8, name only

        [[nodiscard]] std::uint64_t saved() const {
            return original_size > compressed_size ? original_size - compressed_size : 0;
        }
        bool operator==(const StatRecord&) const = default;
    };

    class StatsStore {
    public:
        virtual ~StatsStore() = default;
        [[nodiscard]] virtual std::vector<StatRecord> load() const = 0;
        virtual bool append(const std::vector<StatRecord>& records) = 0;
        virtual bool clear() = 0;
    };

    // tab-separated lines in a utf-8 file, appended
    class FileStatsStore final : public StatsStore {
    public:
        explicit FileStatsStore(std::filesystem::path file);
        [[nodiscard]] std::vector<StatRecord> load() const override;
        bool append(const std::vector<StatRecord>& records) override;
        bool clear() override;

    private:
        std::filesystem::path file_;
    };

    struct StatsSummary {
        std::uint64_t files = 0;
        std::uint64_t original_bytes = 0;
        std::uint64_t saved_bytes = 0;
        std::size_t runs = 0;
        std::optional<StatRecord> biggest; // the record that saved the most bytes
    };

    StatsSummary summarize(const std::vector<StatRecord>& records);

    enum class Period { Day, Month };

    struct PeriodStats {
        std::string key; // local time: "2026-11-21" by day, "2026-11" by month
        StatsSummary summary;
    };

    // oldest period first
    std::vector<PeriodStats> by_period(const std::vector<StatRecord>& records, Period period);

    enum class Phase { Idle, Analyzing, Recompressing };

    // counts over the queued files (not what was found inside them), for the status bar
    struct RunStats {
        std::size_t total = 0;
        std::size_t pending = 0;
        std::size_t processing = 0;
        std::size_t completed = 0;
        std::size_t simulated = 0;
        std::size_t no_gain = 0;
        std::size_t skipped = 0;
        std::size_t errors = 0;
        std::size_t stopped = 0;
        std::uint64_t bytes_before = 0; // of the files completed or simulated
        std::uint64_t bytes_saved = 0;
        std::chrono::milliseconds elapsed{0};
        bool running = false;
        bool stopping = false;
        // the files of the current run, or of the last one
        std::size_t run_total = 0;
        std::size_t run_done = 0;
        std::size_t run_completed = 0;
        std::size_t run_simulated = 0;
        std::size_t run_no_gain = 0;
        std::size_t run_skipped = 0;
        std::size_t run_errors = 0;
        std::size_t run_stopped = 0;
        std::uint64_t run_bytes_total = 0;
        std::uint64_t run_bytes_saved = 0;
        std::uint64_t first_error = 0; // row id of the run's first file in error, 0 if none
        unsigned threads = 0;
        bool dry_run = false;
        Phase phase = Phase::Idle;
    };

} // namespace chisel_app

#endif // CHISEL_APP_STATS_HPP
