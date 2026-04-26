#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

namespace fmp4 {

// Decodes the source MKV's audio track (TrueHD/DTS/AC3/EAC3/FLAC/AAC/...),
// resamples to 48 kHz FLTP stereo, and re-encodes as AAC LC at 128 kbps.
//
// Designed to be reusable across segments: `Reset()` clears the swresample
// fifo and AAC encoder state so a seek can re-anchor `audio_pts_` without
// tearing down the codec contexts.
class AacTranscoder {
public:
    explicit AacTranscoder(AVCodecParameters* in_codecpar);
    ~AacTranscoder();

    AacTranscoder(const AacTranscoder&) = delete;
    AacTranscoder& operator=(const AacTranscoder&) = delete;

    AVCodecContext* encoder_context() const { return enc_ctx_; }
    int initial_padding() const { return initial_padding_; }

    // Feed a source audio packet into the decode→swr pipeline. Decoded PCM is
    // accumulated in internal queues until at least 1024 samples per channel
    // are available, at which point Encode() can run.
    void Decode(AVPacket* in_pkt);

    // Run AAC encoding repeatedly until either the queues drop below 1024
    // samples or the next frame's PTS would exceed `cutoff_pts_48k`.
    // Each emitted AVPacket is appended to `out`. `out` retains ownership of
    // the AVPackets via av_packet_alloc / av_packet_move_ref semantics
    // (caller must av_packet_free for each entry).
    int EncodeUntil(int64_t cutoff_pts_48k, std::vector<AVPacket*>& out);

    // Reset state for a fresh segment span. Sets the next AAC frame's PTS to
    // `next_pts_48k`. Does NOT flush the encoder (AAC encoder doesn't support
    // it cleanly); priming will be handled separately on the first emitted
    // packet of the session.
    void Reset(int64_t next_pts_48k);

    int64_t next_pts() const { return audio_pts_; }

private:
    AVCodecContext* dec_ctx_ = nullptr;
    AVCodecContext* enc_ctx_ = nullptr;
    SwrContext*     swr_ctx_ = nullptr;
    int             initial_padding_ = 1024;

    std::deque<float> left_queue_;
    std::deque<float> right_queue_;
    int64_t           audio_pts_ = 0;     // Next AAC frame input PTS (48 kHz)
};

}  // namespace fmp4
