#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include "EbmlParser.h"
#include "HttpReader.h"

namespace fmp4 {

// Thin wrapper around libavformat that opens an MKV file by HTTP byte-range
// (via HttpReader), exposes Cue-derived keyframe timestamps, supports seeking
// to an exact MKV cluster offset, and yields raw AVPacket pointers for
// downstream consumers (Fmp4Writer for video, AacTranscoder for audio).
//
// Callers are expected to av_packet_unref() returned packets.
class Demuxer {
public:
    explicit Demuxer(const std::string& url);
    ~Demuxer();

    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    int video_stream_index() const { return video_stream_idx_; }
    int audio_stream_index() const { return audio_stream_idx_; }
    int duration_ms() const { return duration_ms_; }

    AVStream* video_stream() const {
        return video_stream_idx_ >= 0 ? fmt_ctx_->streams[video_stream_idx_] : nullptr;
    }
    AVStream* audio_stream() const {
        return audio_stream_idx_ >= 0 ? fmt_ctx_->streams[audio_stream_idx_] : nullptr;
    }

    // Sorted list of segment-aligned keyframe times in ms. The last entry is
    // the end-of-file timestamp so `keyframes[i+1] - keyframes[i]` is the
    // duration of segment `i`.
    std::vector<int64_t> KeyframesMs();

    // Seek to the keyframe at or before `target_ms`. Tears down and rebuilds
    // the AVFormatContext / HttpReader so the EBML demuxer state is fresh —
    // this avoids "Element exceeds containing master element" errors that
    // plain avio_seek can trigger on an active matroska context.
    void SeekToMs(int64_t target_ms);

    // Read the next packet from the demuxer. Returns 0 on success or a
    // negative AVERROR. Caller owns the packet and must av_packet_unref().
    int ReadPacket(AVPacket* pkt);

private:
    void OpenInput();
    void EnsureCueIndex();
    void AddCueIndexEntries();

    std::string url_;
    std::unique_ptr<HttpReader> reader_;
    AVFormatContext* fmt_ctx_ = nullptr;

    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;
    int duration_ms_ = 0;

    std::vector<int64_t> cached_keyframes_;
    std::vector<CueEntry> cached_cues_;
    bool keyframes_cached_ = false;
    bool cue_index_added_ = false;
};

}  // namespace fmp4
