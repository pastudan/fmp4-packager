#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "AacTranscoder.h"
#include "Demuxer.h"

namespace fmp4 {

// Per-(infohash, file_index, session-id) state container.
//
// The session owns a Demuxer (with its own HttpReader / libcurl handle) and
// an AacTranscoder. Per-segment workflow:
//
//   1. Seek the demuxer near segment_start - audio preroll using cue metadata.
//   2. Walk packets until the next keyframe past segment_end_ms, splitting
//      segment video Samples and caching decoded -> re-encoded AAC by global
//      1024-sample frame index.
//   3. Emit only this segment's half-open AAC frame range, so forward seeks do
//      not require audio from earlier segments.
//   4. Hand all of that to Fmp4Writer::BuildSegment
//   5. Cache the result
class Session {
public:
    Session(const std::string& webtorrent_url,
            const std::string& infohash,
            int file_index,
            int session_id);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    std::vector<int64_t> GetKeyframesMs();
    int GetDurationMs() const { return demuxer_ ? demuxer_->duration_ms() : 0; }

    std::vector<uint8_t> GenerateInitSegment();
    std::vector<uint8_t> GenerateSegment(int64_t segment_idx,
                                         int64_t segment_start_ms,
                                         int64_t segment_end_ms);

    void Touch() { last_activity_ = std::chrono::steady_clock::now(); }
    std::chrono::steady_clock::time_point GetLastActivity() const { return last_activity_; }

private:
    std::vector<uint8_t> BuildSegmentLocked(int64_t segment_idx,
                                            int64_t segment_start_ms,
                                            int64_t segment_end_ms);

    std::string url_;
    std::string short_infohash_;  // first 8 chars, used for debug logging
    int session_id_ = 0;
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<AacTranscoder> aac_;
    AVCodecParameters* aac_codecpar_ = nullptr;  // populated after AAC encoder open

    // Track templates filled in once on first init build, then reused.
    bool init_built_ = false;
    std::vector<uint8_t> init_cache_;

    // Segment cache so re-fetches return byte-identical output.
    std::unordered_map<int64_t, std::vector<uint8_t>> segment_cache_;
    static constexpr size_t kMaxCachedSegments = 50;

    // Encoded AAC payload cache keyed by global 1024-sample frame index.
    std::unordered_map<int64_t, std::vector<uint8_t>> audio_frame_cache_;
    static constexpr size_t kMaxCachedAudioFrames = 4096;

    std::chrono::steady_clock::time_point last_activity_;
    std::mutex mutex_;
};

}  // namespace fmp4
