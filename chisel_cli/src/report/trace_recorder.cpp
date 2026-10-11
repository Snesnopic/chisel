//
// Created by Giuseppe Francione on 10/10/26.
//

#include "trace_recorder.hpp"
#include "event_bus.hpp"
#include "events.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <ranges>
#include <string_view>

using namespace chisel;

namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

std::string json_string(const std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

} // namespace

void TraceRecorder::subscribe(EventBus& bus) {
    bus.subscribe<FileAnalyzeStartEvent>([this](const FileAnalyzeStartEvent& e) {
        begin("analyze", e.path);
    });
    bus.subscribe<FileAnalyzeCompleteEvent>([this](const FileAnalyzeCompleteEvent& e) {
        end("analyze", e.path, e.extracted ? "extracted " + std::to_string(e.num_children) + " files" : "scheduled",
            std::nullopt, e.parent);
    });
    bus.subscribe<FileAnalyzeSkippedEvent>([this](const FileAnalyzeSkippedEvent& e) {
        end("analyze", e.path, e.reason);
    });
    bus.subscribe<FileAnalyzeErrorEvent>([this](const FileAnalyzeErrorEvent& e) {
        end("analyze", e.path, "error: " + e.error_message);
    });

    bus.subscribe<FileProcessStartEvent>([this](const FileProcessStartEvent& e) {
        begin("process", e.path, e.parent_container);
    });
    bus.subscribe<FileProcessCompleteEvent>([this](const FileProcessCompleteEvent& e) {
        end("process", e.path, e.replaced ? "replaced" : "kept", e.new_size);
    });
    bus.subscribe<FileProcessSkippedEvent>([this](const FileProcessSkippedEvent& e) {
        end("process", e.path, e.reason);
    });
    bus.subscribe<FileProcessErrorEvent>([this](const FileProcessErrorEvent& e) {
        end("process", e.path, "error: " + e.error_message);
    });

    bus.subscribe<ContainerFinalizeStartEvent>([this](const ContainerFinalizeStartEvent& e) {
        begin("finalize", e.path);
    });
    bus.subscribe<ContainerFinalizeCompleteEvent>([this](const ContainerFinalizeCompleteEvent& e) {
        end("finalize", e.path, e.replaced ? "replaced" : "kept", e.final_size);
    });
    bus.subscribe<ContainerFinalizeErrorEvent>([this](const ContainerFinalizeErrorEvent& e) {
        end("finalize", e.path, "error: " + e.error_message);
    });
}

std::int64_t TraceRecorder::now_us() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - origin_).count();
}

void TraceRecorder::begin(const char* phase, const std::filesystem::path& path,
                          std::optional<std::filesystem::path> parent) {
    const auto start = now_us();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    std::scoped_lock lock(mtx_);
    const auto [thread, inserted] = threads_.try_emplace(std::this_thread::get_id(),
                                                         static_cast<unsigned>(threads_.size()));
    open_[{phase, path}] = Span{.phase=phase, .path=path, .thread=thread->second, .start_us=start,
                                .size=ec ? 0 : size, .parent=std::move(parent)};
}

void TraceRecorder::end(const char* phase, const std::filesystem::path& path, std::string outcome,
                        const std::optional<std::uintmax_t> new_size, std::optional<std::filesystem::path> parent) {
    const auto stop = now_us();
    std::scoped_lock lock(mtx_);
    // skips and errors can also come for tasks that never started
    const auto it = open_.find({phase, path});
    if (it == open_.end()) return;
    Span span = std::move(it->second);
    open_.erase(it);
    span.duration_us = stop - span.start_us;
    span.outcome = std::move(outcome);
    span.new_size = new_size;
    if (parent) span.parent = std::move(parent);
    spans_.push_back(std::move(span));
}

bool TraceRecorder::write(const std::filesystem::path& path) const {
    std::scoped_lock lock(mtx_);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    out << R"({"displayTimeUnit":"ms","traceEvents":[)" << '\n'
        << R"({"name":"process_name","ph":"M","pid":1,"tid":0,"args":{"name":"chsl"}})";
    for (const auto& index : threads_ | std::views::values) {
        out << ",\n" << R"({"name":"thread_name","ph":"M","pid":1,"tid":)" << index
            << R"(,"args":{"name":"worker )" << index << "\"}}";
    }

    std::vector<const Span*> sorted;
    sorted.reserve(spans_.size());
    for (const auto& s : spans_) sorted.push_back(&s);
    std::ranges::sort(sorted, {}, &Span::start_us);
    for (const Span* s : sorted) {
        out << ",\n{\"name\":" << json_string(utf8(s->path.filename())) << ",\"cat\":\"" << s->phase
            << R"(","ph":"X","ts":)" << s->start_us << ",\"dur\":" << s->duration_us
            << R"(,"pid":1,"tid":)" << s->thread << ",\"args\":{\"path\":" << json_string(utf8(s->path))
            << ",\"size\":" << s->size;
        if (s->new_size) out << ",\"new_size\":" << *s->new_size;
        if (s->parent) out << ",\"parent\":" << json_string(utf8(*s->parent));
        out << ",\"outcome\":" << json_string(s->outcome) << "}}";
    }
    out << "\n]}\n";
    return static_cast<bool>(out);
}
