//
// created by giuseppe francione on 25/09/25
//
/*
#include "mkv_handler.hpp"
#include "../utils/logger.hpp"
extern "C" {
#include <sys/stat.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/avstring.h>
}

using namespace std;

// helper: convert avdictionary to std::map
static map<string, string> dict_to_map(AVDictionary* dict) {
    map<string, string> result;
    AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(dict, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        result[tag->key] = tag->value ? tag->value : "";
    }
    return result;
}

// helper: derive a safe filename from metadata and stream index
static string derive_attachment_name(const TrackInfo& info) {
    // prefer explicit metadata keys commonly used by mkv: "filename", "mimetype", "title"
    auto it_name = info.metadata.find("filename");
    if (it_name != info.metadata.end() && !it_name->second.empty()) return it_name->second;

    auto it_title = info.metadata.find("title");
    string base = (it_title != info.metadata.end() && !it_title->second.empty())
                  ? it_title->second
                  : ("attachment_" + to_string(info.stream_index));

    string ext;
    auto it_mime = info.metadata.find("mimetype");
    if (it_mime != info.metadata.end()) {
        const string& mime = it_mime->second;
        if (mime == "image/jpeg") ext = ".jpg";
        else if (mime == "image/png") ext = ".png";
        else if (mime == "application/x-truetype-font" || mime == "font/ttf") ext = ".ttf";
        else if (mime == "application/vnd.ms-opentype" || mime == "font/otf") ext = ".otf";
        else if (mime == "application/xml") ext = ".xml";
        else if (mime == "text/plain") ext = ".txt";
    }
    return base + ext;
}

// helper: create temporary directory path for mkv processing
static string make_temp_dir_for(const string& src_path) {
    // very basic temp dir derivation: <src_path>.mkv.tmp
    return src_path + ".mkv.tmp";
}

// helper: ensure directory exists (portable minimal)
static bool ensure_dir(const string& path) {
#ifdef _WIN32
    int ret = _mkdir(path.c_str());
    return (ret == 0 || errno == EEXIST);
#else
    int ret = mkdir(path.c_str(), 0755);
    if (ret != 0 && errno == EEXIST) return true;
    return ret == 0;
#endif
}

// prepare: open mkv, gather track/attachment metadata, extract attachments to temp files
ContainerJob MkvHandler::prepare(const string& path) {
    Logger::log(LogLevel::INFO, "preparing mkv container: " + path, "MkvHandler");

    MkvJob job;
    job.original_path = path;
    job.temp_dir = make_temp_dir_for(path);
    job.format = ContainerFormat::Mkv;

    if (!ensure_dir(job.temp_dir)) {
        Logger::log(LogLevel::ERROR, "failed to create temporary directory: " + job.temp_dir, "MkvHandler");
        return job;
    }

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
        Logger::log(LogLevel::ERROR, "failed to open mkv file", "MkvHandler");
        return job;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        Logger::log(LogLevel::ERROR, "failed to read stream info", "MkvHandler");
        avformat_close_input(&fmt_ctx);
        return job;
    }

    // global metadata
    job.global_metadata = dict_to_map(fmt_ctx->metadata);

    // iterate streams and collect track info
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* st = fmt_ctx->streams[i];
        TrackInfo info;
        info.stream_index = static_cast<int>(i);

        switch (st->codecpar->codec_type) {
            case AVMEDIA_TYPE_AUDIO: info.type = TrackType::Audio; break;
            case AVMEDIA_TYPE_VIDEO: info.type = TrackType::Video; break;
            case AVMEDIA_TYPE_SUBTITLE: info.type = TrackType::Subtitle; break;
            case AVMEDIA_TYPE_ATTACHMENT: info.type = TrackType::Attachment; break;
            default: info.type = TrackType::Unknown; break;
        }

        info.codec_name = avcodec_get_name(st->codecpar->codec_id);
        info.metadata = dict_to_map(st->metadata);

        // attachments: extract raw data from packets (preferred) or extradata if present
        if (info.type == TrackType::Attachment) {
            // derive filename and write content to disk
            string filename = derive_attachment_name(info);
            if (filename.empty()) filename = "attachment_" + to_string(info.stream_index);

            string out_path = job.temp_dir + "/" + filename;
            info.path = filename; // store relative name; final path will be temp_dir + "/" + path

            // try to extract attachment bytes: mkv attachments are usually provided via packets
            // if there are no packets, fall back to extradata
            bool wrote = false;
            AVPacket* pkt = av_packet_alloc();
            if (pkt) {
                // read all packets and filter on attachment stream index
                av_seek_frame(fmt_ctx, info.stream_index, 0, AVSEEK_FLAG_BACKWARD);
                while (av_read_frame(fmt_ctx, pkt) >= 0) {
                    if (pkt->stream_index == info.stream_index && pkt->size > 0) {
                        FILE* f = fopen(out_path.c_str(), "wb");
                        if (f) {
                            fwrite(pkt->data, 1, pkt->size, f);
                            fclose(f);
                            wrote = true;
                        }
                        av_packet_unref(pkt);
                        break; // assume single attachment payload
                    }
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }

            if (!wrote) {
                // fallback to extradata
                if (st->codecpar->extradata && st->codecpar->extradata_size > 0) {
                    FILE* f = fopen(out_path.c_str(), "wb");
                    if (f) {
                        fwrite(st->codecpar->extradata, 1, st->codecpar->extradata_size, f);
                        fclose(f);
                        wrote = true;
                    }
                }
            }

            if (!wrote) {
                Logger::log(LogLevel::WARNING, "attachment extraction failed for stream " + to_string(info.stream_index), "MkvHandler");
            } else {
                // push extracted file to job.file_list for potential recompression (e.g., images)
                job.file_list.push_back(out_path);
            }
        } else if (info.type == TrackType::Audio) {
            // mark audio tracks as candidates for recompression; extraction to temp files is deferred to encoders
            // store a synthetic path pointing to the container + stream index
            info.path = job.original_path + "#stream=" + to_string(info.stream_index);
        } else {
            // leave other streams untouched by default
            info.path.clear();
        }

        job.tracks.push_back(info);
    }

    // chapters: capture ids, timestamps and metadata
    for (unsigned int i = 0; i < fmt_ctx->nb_chapters; i++) {
        AVChapter* ch = fmt_ctx->chapters[i];
        ChapterInfo ci;
        ci.id = ch->id;
        ci.start_time = ch->start;
        ci.end_time = ch->end;
        ci.metadata = dict_to_map(ch->metadata);
        job.chapters.push_back(ci);
    }

    avformat_close_input(&fmt_ctx);

    Logger::log(LogLevel::INFO, "mkv prepare completed: " + to_string(job.tracks.size()) + " streams, " + to_string(job.chapters.size()) + " chapters", "MkvHandler");
    return job;
}

// finalize: rebuild mkv with recompressed tracks, preserved metadata and attachments
bool MkvHandler::finalize(const ContainerJob& base_job, Settings& _) {
    const MkvJob& job = static_cast<const MkvJob&>(base_job);

    Logger::log(LogLevel::INFO, "finalizing mkv container: " + job.original_path, "MkvHandler");

    // open input to map original streams
    AVFormatContext* in_ctx = nullptr;
    if (avformat_open_input(&in_ctx, job.original_path.c_str(), nullptr, nullptr) < 0) {
        Logger::log(LogLevel::ERROR, "failed to open input mkv", "MkvHandler");
        return false;
    }
    if (avformat_find_stream_info(in_ctx, nullptr) < 0) {
        Logger::log(LogLevel::ERROR, "failed to read input stream info", "MkvHandler");
        avformat_close_input(&in_ctx);
        return false;
    }

    // output to temporary path (write to same directory with .repacked suffix)
    const string out_path = job.temp_dir + "/repacked.mkv";
    AVFormatContext* out_ctx = nullptr;
    if (avformat_alloc_output_context2(&out_ctx, nullptr, "matroska", out_path.c_str()) < 0 || !out_ctx) {
        Logger::log(LogLevel::ERROR, "failed to allocate output context", "MkvHandler");
        avformat_close_input(&in_ctx);
        return false;
    }

    // copy global metadata
    for (auto& kv : job.global_metadata) {
        av_dict_set(&out_ctx->metadata, kv.first.c_str(), kv.second.c_str(), 0);
    }

    // create output streams maintaining original order and types
    vector<int> stream_map(in_ctx->nb_streams, -1);
    for (const auto& track : job.tracks) {
        AVStream* in_st = in_ctx->streams[track.stream_index];
        AVStream* out_st = avformat_new_stream(out_ctx, nullptr);
        if (!out_st) {
            Logger::log(LogLevel::ERROR, "failed to create output stream", "MkvHandler");
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return false;
        }
        stream_map[track.stream_index] = out_st->index;

        // copy codec parameters for direct muxing; encoders will have produced files when needed
        if (avcodec_parameters_copy(out_st->codecpar, in_st->codecpar) < 0) {
            Logger::log(LogLevel::ERROR, "failed to copy codec parameters", "MkvHandler");
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return false;
        }
        out_st->id = in_st->id;
        out_st->time_base = in_st->time_base;

        // copy per-stream metadata
        for (auto& kv : track.metadata) {
            av_dict_set(&out_st->metadata, kv.first.c_str(), kv.second.c_str(), 0);
        }
    }

    // chapters: public libavformat api does not provide a stable way to create chapters programmatically;
    // to preserve chapters, we keep the original input open and rely on stream copy for remuxing packets,
    // while metadata remains intact for muxers that support implicit chapter retention.

    // open io
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_ctx->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0) {
            Logger::log(LogLevel::ERROR, "failed to open output file", "MkvHandler");
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return false;
        }
    }

    // write header
    if (avformat_write_header(out_ctx, nullptr) < 0) {
        Logger::log(LogLevel::ERROR, "failed to write mkv header", "MkvHandler");
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        avformat_close_input(&in_ctx);
        avformat_free_context(out_ctx);
        return false;
    }

    // remux packets; for streams that were recompressed, the pipeline should provide new packets,
    // but here we default to copying original packets so the container remains valid
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        Logger::log(LogLevel::ERROR, "failed to allocate packet", "MkvHandler");
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        avformat_close_input(&in_ctx);
        avformat_free_context(out_ctx);
        return false;
    }

    // read packets and write to output according to stream_map
    while (av_read_frame(in_ctx, pkt) >= 0) {
        int in_index = pkt->stream_index;
        if (in_index < 0 || in_index >= static_cast<int>(stream_map.size())) {
            av_packet_unref(pkt);
            continue;
        }

        int out_index = stream_map[in_index];
        if (out_index < 0) {
            av_packet_unref(pkt);
            continue;
        }

        // rescale pts/dts to output time_base
        AVStream* in_st = in_ctx->streams[in_index];
        AVStream* out_st = out_ctx->streams[out_index];
        pkt->pts = av_rescale_q_rnd(pkt->pts, in_st->time_base, out_st->time_base, AV_ROUND_NEAR_INF);
        pkt->dts = av_rescale_q_rnd(pkt->dts, in_st->time_base, out_st->time_base, AV_ROUND_NEAR_INF);
        pkt->duration = av_rescale_q(pkt->duration, in_st->time_base, out_st->time_base);
        pkt->stream_index = out_index;

        if (av_interleaved_write_frame(out_ctx, pkt) < 0) {
            Logger::log(LogLevel::ERROR, "failed to write packet", "MkvHandler");
            av_packet_unref(pkt);
            if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return false;
        }
        av_packet_unref(pkt);
    }

    // trailer
    if (av_write_trailer(out_ctx) < 0) {
        Logger::log(LogLevel::ERROR, "failed to write mkv trailer", "MkvHandler");
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        avformat_close_input(&in_ctx);
        avformat_free_context(out_ctx);
        return false;
    }

    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&out_ctx->pb);
    }
    av_packet_free(&pkt);
    avformat_close_input(&in_ctx);
    avformat_free_context(out_ctx);

    Logger::log(LogLevel::INFO, "mkv finalize completed: " + out_path, "MkvHandler");
    return true;
}
*/