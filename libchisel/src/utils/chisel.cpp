//
// Created by Giuseppe Francione on 09/12/25.
//

/**
 * @file chisel.cpp
 * @brief Implementation of the public Chisel API.
 */

#include "../include/chisel.hpp"

#include "../include/processor_registry.hpp"
#include "../include/processor_executor.hpp"
#include "../include/event_bus.hpp"
#include "../include/logger.hpp"
#include "../include/log_sink.hpp"

#include <mutex>
#include <thread>
#include <algorithm>

#include "events.hpp"
#include "file_type.hpp"

namespace chisel {

// bridge sink to redirect static logs to the instance observer
class BridgeLogSink final : public ILogSink {
    ChiselObserver* observer_;
public:
    explicit BridgeLogSink(ChiselObserver* obs) : observer_(obs) {}

    void log(const LogLevel level, const std::string_view message, const std::string_view tag) override {
        if (observer_) {
            observer_->onLog(static_cast<int>(level), std::string(message), std::string(tag));
        }
    }
};

struct Chisel::Impl {
    ProcessorRegistry registry;
    EventBus eventBus;

    bool preserveMetadata = true;
    bool verifyChecksums = false;
    bool dryRun = false;
    unsigned numThreads = std::thread::hardware_concurrency() / 2;
    EncodeMode encodeMode = EncodeMode::PIPE;
    std::filesystem::path outputDir;

    ChiselObserver* observer = nullptr;
    std::atomic<ProcessorExecutor*> currentExecutor = nullptr;

    Impl() {
        if (numThreads == 0) numThreads = 1;
    }

    // map public enum to internal global enum
    ::EncodeMode getInternalMode() const {
        switch (encodeMode) {
            case EncodeMode::PIPE: return ::EncodeMode::PIPE;
            case EncodeMode::PARALLEL: return ::EncodeMode::PARALLEL;
            default: return ::EncodeMode::PIPE;
        }
    }

    void setupEventBridging() {
        if (!observer) return;

        eventBus.subscribe<FileProcessStartEvent>([this](const FileProcessStartEvent& e) {
            observer->onFileStart(e.path);
        });

        eventBus.subscribe<FileProcessCompleteEvent>([this](const FileProcessCompleteEvent& e) {
            observer->onFileFinish(e.path, e.original_size, e.new_size, e.replaced);
        });

        eventBus.subscribe<FileProcessErrorEvent>([this](const FileProcessErrorEvent& e) {
            observer->onFileError(e.path, e.error_message);
        });

        eventBus.subscribe<FileProcessSkippedEvent>([this](const FileProcessSkippedEvent& e) {
            // skipped implies success but no replacement
            observer->onFileFinish(e.path, 0, 0, false);
        });

        eventBus.subscribe<ContainerFinalizeErrorEvent>([this](const ContainerFinalizeErrorEvent& e) {
            observer->onFileError(e.path, "Container finalize error: " + e.error_message);
        });
    }
};

Chisel::Chisel() : impl_(std::make_unique<Impl>()) {}

Chisel::~Chisel() {
    stop();
}

Chisel::Chisel(Chisel&&) noexcept = default;
Chisel& Chisel::operator=(Chisel&&) noexcept = default;

Chisel& Chisel::preserveMetadata(bool val) {
    impl_->preserveMetadata = val;
    return *this;
}

Chisel& Chisel::verifyChecksums(bool val) {
    impl_->verifyChecksums = val;
    return *this;
}

Chisel& Chisel::dryRun(bool val) {
    impl_->dryRun = val;
    return *this;
}

Chisel& Chisel::threads(unsigned val) {
    impl_->numThreads = val > 0 ? val : std::thread::hardware_concurrency() / 2;
    if (impl_->numThreads == 0) impl_->numThreads = 1;
    return *this;
}

Chisel& Chisel::mode(EncodeMode m) {
    impl_->encodeMode = m;
    return *this;
}

Chisel& Chisel::outputDirectory(const std::filesystem::path& dir) {
    impl_->outputDir = dir;
    return *this;
}

void Chisel::setObserver(ChiselObserver* observer) {
    impl_->observer = observer;
}

void Chisel::recompress(const std::vector<std::filesystem::path>& paths) {
    impl_->setupEventBridging();

    // inject bridge sink if observer is present
    if (impl_->observer) {
        auto sink = std::make_unique<BridgeLogSink>(impl_->observer);
        Logger::add_sink(std::move(sink));
    }

    ProcessorExecutor executor(
        impl_->registry,
        impl_->preserveMetadata,
        impl_->verifyChecksums,
        static_cast<EncodeMode>(impl_->getInternalMode()),
        impl_->dryRun,
        impl_->outputDir,
        impl_->eventBus,
        impl_->numThreads
    );

    impl_->currentExecutor.store(&executor);

    try {
        executor.process(paths);
    } catch (...) {
        impl_->currentExecutor.store(nullptr);
        throw;
    }

    impl_->currentExecutor.store(nullptr);
}

void Chisel::recompress(const std::filesystem::path& path) {
    recompress(std::vector<std::filesystem::path>{path});
}

void Chisel::recompress(const std::vector<std::string>& paths) {
    std::vector<std::filesystem::path> fs_paths;
    fs_paths.reserve(paths.size());
    for (const auto& p : paths) {
        fs_paths.emplace_back(p);
    }
    recompress(fs_paths);
}

void Chisel::stop() {
    auto* exec = impl_->currentExecutor.load();
    if (exec) {
        exec->request_stop();
    }
}

} // namespace chisel