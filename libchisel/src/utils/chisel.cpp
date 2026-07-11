//
// Created by Giuseppe Francione on 09/12/25.
//

/**
 * @file chisel.cpp
 * @brief Implementation of the public Chisel API.
 */

#include "chisel.hpp"


namespace chisel {

// bridge sink to redirect static logs to the instance observer
class BridgeLogSink final : public ILogSink {
    ChiselObserver* observer_;
public:
    explicit BridgeLogSink(ChiselObserver* obs) : observer_(obs) {}

    void log(const LogLevel level, const std::string_view message, const std::string_view tag) override {
        if (observer_ != nullptr) {
            observer_->onLog(static_cast<int>(level), std::string(message), std::string(tag));
        }
    }
};

struct Chisel::Impl {
    ProcessorRegistry registry;
    EventBus eventBus;

    ProcessingOptions options;

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
    EncodeMode getInternalMode() const {
        switch (encodeMode) {
            case EncodeMode::PIPE: return EncodeMode::PIPE;
            case EncodeMode::PARALLEL: return EncodeMode::PARALLEL;
            default: return EncodeMode::PIPE;
        }
    }

    void setupEventBridging() {
        if (observer == nullptr) return;

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

Chisel& Chisel::preserveMetadata(const bool val) {
    impl_->options.preserve_metadata = val;
    return *this;
}

Chisel& Chisel::verifyChecksums(const bool val) {
    impl_->options.verify_checksums = val;
    return *this;
}

Chisel& Chisel::dryRun(const bool val) {
    impl_->dryRun = val;
    return *this;
}

Chisel& Chisel::threads(const unsigned val) {
    impl_->numThreads = val > 0 ? val : std::thread::hardware_concurrency() / 2;
    if (impl_->numThreads == 0) impl_->numThreads = 1;
    return *this;
}

bool Chisel::isCompatible(const std::filesystem::path& path) const {
    std::error_code ec;

    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return false;
    }

    try {
        return impl_->registry.supports_mime(MimeDetector::detect(path));
    } catch (const std::exception&) {
        // fallback for unreadable files or detection failures
        return false;
    }
}

std::set<std::string_view> Chisel::supportedExtensions() const {
    const auto& procs = impl_->registry.all();
    std::set<std::string_view> extensions;

    for (const auto& proc : procs) {
        const auto& exts = proc->get_supported_extensions();

        extensions.insert(exts.begin(), exts.end());
    }

    return extensions;
}

std::set<std::string_view> Chisel::supportedMimeTypes() const {
    const auto& procs = impl_->registry.all();
    std::set<std::string_view> mimes;

    for (const auto& proc : procs) {
        const auto& exts = proc->get_supported_mime_types();

        mimes.insert(exts.begin(), exts.end());
    }

    return mimes;
}

std::string_view Chisel::version() {
    return CHISEL_VERSION;
}

Chisel& Chisel::mode(const EncodeMode m) {
    impl_->encodeMode = m;
    return *this;
}

Chisel& Chisel::outputDirectory(const std::filesystem::path& dir) {
    impl_->outputDir = dir;
    return *this;
}

void Chisel::setObserver(ChiselObserver* observer) const {
    impl_->observer = observer;
}

void Chisel::recompress(const std::vector<std::filesystem::path>& paths) const {
    impl_->setupEventBridging();

    // inject bridge sink if observer is present
    std::unique_ptr<BridgeLogSink> owned_sink;
    ILogSink* sink_ptr = nullptr;
    if (impl_->observer) {
        owned_sink = std::make_unique<BridgeLogSink>(impl_->observer);
        sink_ptr = owned_sink.get();
        Logger::add_sink(std::move(owned_sink));
    }
    struct SinkGuard {
        ILogSink* sink;
        ~SinkGuard() { if (sink) Logger::remove_sink(sink); }
    } sink_guard{sink_ptr};

    ProcessorExecutor executor(
        impl_->registry,
        impl_->options,
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

void Chisel::recompress(const std::filesystem::path& path) const {
    recompress(std::vector<std::filesystem::path>{path});
}

void Chisel::recompress(const std::vector<std::string>& paths) const {
    std::vector<std::filesystem::path> fs_paths;
    fs_paths.reserve(paths.size());
    for (const auto& p : paths) {
        fs_paths.emplace_back(p);
    }
    recompress(fs_paths);
}

void Chisel::stop() const {
    auto* exec = impl_->currentExecutor.load();
    if (exec != nullptr) {
        exec->request_stop();
    }
}

} // namespace chisel