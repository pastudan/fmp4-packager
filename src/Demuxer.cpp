#include "Demuxer.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace fmp4 {

namespace {

// Filter raw cue keyframes to ~10 second segment boundaries: pick the first
// keyframe at or after each multiple of `target_duration_ms`. Mirrors
// FFmpeg's `-hls_time` selection so segment durations land near the target.
std::vector<int64_t> FilterKeyframes(const std::vector<int64_t>& raw,
                                     int64_t target_duration_ms = 10000) {
    if (raw.size() < 2) return raw;

    std::vector<int64_t> filtered;
    filtered.push_back(raw[0]);

    int64_t next_target = target_duration_ms;
    for (size_t i = 1; i < raw.size(); ++i) {
        if (raw[i] >= next_target) {
            filtered.push_back(raw[i]);
            while (next_target <= raw[i]) next_target += target_duration_ms;
        }
    }
    if (filtered.back() != raw.back()) {
        filtered.push_back(raw.back());
    }
    return filtered;
}

}  // namespace

Demuxer::Demuxer(const std::string& url) : url_(url) {
    auto start = std::chrono::steady_clock::now();
    std::cout << "[Demuxer] Opening " << url_ << std::endl;

    reader_ = std::make_unique<HttpReader>(url_);
    OpenInput();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "[Demuxer] Opened in " << elapsed << "ms (duration="
              << duration_ms_ << "ms)" << std::endl;
}

Demuxer::~Demuxer() {
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
}

void Demuxer::OpenInput() {
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) throw std::runtime_error("avformat_alloc_context failed");

    fmt_ctx_->pb = reader_->GetAvioContext();
    fmt_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt_ctx_->probesize = 32 * 1024;
    fmt_ctx_->max_analyze_duration = 0;

    int ret = avformat_open_input(&fmt_ctx_, nullptr, nullptr, nullptr);
    if (ret < 0) {
        char err[256];
        av_make_error_string(err, sizeof(err), ret);
        throw std::runtime_error(std::string("avformat_open_input: ") + err);
    }

    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) throw std::runtime_error("avformat_find_stream_info failed");

    video_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_stream_idx_ < 0) throw std::runtime_error("No video stream found");

    duration_ms_ = static_cast<int>(fmt_ctx_->duration * 1000 / AV_TIME_BASE);

    av_seek_frame(fmt_ctx_, -1, 0, AVSEEK_FLAG_BACKWARD);
}

void Demuxer::EnsureCueIndex() {
    if (keyframes_cached_) {
        AddCueIndexEntries();
        return;
    }

    int64_t saved_pos = avio_tell(fmt_ctx_->pb);
    avio_seek(fmt_ctx_->pb, 0, SEEK_SET);

    constexpr int kHeaderSize = 256 * 1024;
    std::vector<uint8_t> header(kHeaderSize);
    int bytes_read = avio_read(fmt_ctx_->pb, header.data(), kHeaderSize);

    if (bytes_read <= 0) {
        avio_seek(fmt_ctx_->pb, saved_pos, SEEK_SET);
        cached_keyframes_.clear();
        keyframes_cached_ = true;
        return;
    }

    auto seg_info  = parse_segment_info(header.data(), bytes_read);
    auto cues_loc  = parse_cues_location(header.data(), bytes_read);

    if (!cues_loc.has_value()) {
        std::cerr << "[Demuxer] Cues location not found in EBML header" << std::endl;
        avio_seek(fmt_ctx_->pb, saved_pos, SEEK_SET);
        cached_keyframes_.clear();
        keyframes_cached_ = true;
        return;
    }

    avio_seek(fmt_ctx_->pb, cues_loc->offset, SEEK_SET);
    std::vector<uint8_t> hdr_chunk(32);
    int header_read = avio_read(fmt_ctx_->pb, hdr_chunk.data(), 32);
    if (header_read < 12) {
        avio_seek(fmt_ctx_->pb, saved_pos, SEEK_SET);
        cached_keyframes_.clear();
        keyframes_cached_ = true;
        return;
    }

    uint64_t cues_id = 0;
    int64_t  cues_element_size = 0;
    int id_bytes = ebml_read_id(hdr_chunk.data(), header_read, cues_id);
    if (id_bytes == 0 || cues_id != kEbmlId_Cues) {
        avio_seek(fmt_ctx_->pb, saved_pos, SEEK_SET);
        cached_keyframes_.clear();
        keyframes_cached_ = true;
        return;
    }

    int size_bytes = ebml_read_size(hdr_chunk.data() + id_bytes, header_read - id_bytes,
                                    cues_element_size);
    int64_t total_cues_size = id_bytes + size_bytes + cues_element_size;
    if (total_cues_size > 32 * 1024 * 1024) total_cues_size = 32 * 1024 * 1024;

    avio_seek(fmt_ctx_->pb, cues_loc->offset, SEEK_SET);
    std::vector<uint8_t> cues_buf(total_cues_size);
    int cues_bytes = avio_read(fmt_ctx_->pb, cues_buf.data(), total_cues_size);

    avio_seek(fmt_ctx_->pb, saved_pos, SEEK_SET);

    if (cues_bytes <= 0) {
        cached_keyframes_.clear();
        keyframes_cached_ = true;
        return;
    }

    auto cue_entries = parse_cues_data(cues_buf.data(), cues_bytes,
                                       seg_info.timestamp_scale,
                                       seg_info.video_track_number,
                                       cues_loc->segment_data_start);

    std::vector<int64_t> raw_keyframes;
    raw_keyframes.reserve(cue_entries.size());
    for (const auto& e : cue_entries) raw_keyframes.push_back(e.time_ms);

    auto filtered = FilterKeyframes(raw_keyframes, 10000);

    cached_keyframes_ = std::move(filtered);
    cached_cues_      = std::move(cue_entries);
    keyframes_cached_ = true;
    std::cout << "[Demuxer] Cached " << cached_keyframes_.size()
              << " keyframe segment boundaries from " << cached_cues_.size() << " cue entries"
              << std::endl;
    AddCueIndexEntries();
}

void Demuxer::AddCueIndexEntries() {
    if (cue_index_added_ || cached_cues_.empty() || video_stream_idx_ < 0) return;

    AVStream* video = fmt_ctx_->streams[video_stream_idx_];
    for (const auto& cue : cached_cues_) {
        if (cue.cluster_pos < 0) continue;
        int64_t ts = av_rescale_q(cue.time_ms, AVRational{1, 1000}, video->time_base);
        av_add_index_entry(video, cue.cluster_pos, ts, 0, 0, AVINDEX_KEYFRAME);
    }
    cue_index_added_ = true;
}

std::vector<int64_t> Demuxer::KeyframesMs() {
    EnsureCueIndex();
    return cached_keyframes_;
}

void Demuxer::SeekToMs(int64_t target_ms) {
    EnsureCueIndex();

    AVStream* video = fmt_ctx_->streams[video_stream_idx_];
    int64_t target_ts = av_rescale_q(target_ms, AVRational{1, 1000}, video->time_base);

    // Find the latest cue at or before target_ms.
    const CueEntry* cue = nullptr;
    for (const auto& entry : cached_cues_) {
        if (entry.time_ms <= target_ms) cue = &entry;
        else break;
    }

    // Tear down the AVIO/Matroska state and reopen; a raw avio_seek on an
    // active matroska context can leave the EBML level stack stale and
    // produce "Element exceeds containing master element" errors on the
    // first packet read.
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
    reader_ = std::make_unique<HttpReader>(url_);
    cue_index_added_ = false;
    OpenInput();
    AddCueIndexEntries();

    if (cue && cue->cluster_pos >= 0) {
        avformat_flush(fmt_ctx_);
        avio_seek(fmt_ctx_->pb, cue->cluster_pos, SEEK_SET);
        return;
    }

    int ret = avformat_seek_file(fmt_ctx_, video_stream_idx_,
                                 INT64_MIN, target_ts, target_ts,
                                 AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        std::cerr << "[Demuxer] avformat_seek_file failed for ms=" << target_ms << std::endl;
    }
    avformat_flush(fmt_ctx_);
}

int Demuxer::ReadPacket(AVPacket* pkt) {
    return av_read_frame(fmt_ctx_, pkt);
}

}  // namespace fmp4
