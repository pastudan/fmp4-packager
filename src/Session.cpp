#include "Session.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include "Fmp4Writer.h"

namespace fmp4 {

namespace {

constexpr AVRational kVideoOutTb = {1, 16000};
constexpr AVRational kAudioOutTb = {1, 48000};
constexpr int64_t kAudioPrerollMs = 2000;
constexpr int64_t kAudioLookaheadMs = 250;

int64_t FloorDiv(int64_t num, int64_t den) {
    int64_t q = num / den;
    int64_t r = num % den;
    if (r != 0 && ((r < 0) != (den < 0))) --q;
    return q;
}

int64_t MsToAudioPts(int64_t ms) {
    return av_rescale_q(ms, AVRational{1, 1000}, kAudioOutTb);
}

int64_t AudioFrameIndexForPts(int64_t pts_48k) {
    return FloorDiv(pts_48k, 1024);
}

}  // namespace

Session::Session(const std::string& webtorrent_url,
                 const std::string& infohash,
                 int file_index)
    : url_(webtorrent_url + "/files/" + infohash + "/" + std::to_string(file_index))
    , last_activity_(std::chrono::steady_clock::now()) {
    demuxer_ = std::make_unique<Demuxer>(url_);
    if (demuxer_->audio_stream_index() >= 0) {
        AVStream* astream = demuxer_->audio_stream();
        aac_ = std::make_unique<AacTranscoder>(astream->codecpar);
        aac_codecpar_ = avcodec_parameters_alloc();
        avcodec_parameters_from_context(aac_codecpar_, aac_->encoder_context());
    }
}

Session::~Session() {
    if (aac_codecpar_) avcodec_parameters_free(&aac_codecpar_);
}

std::vector<int64_t> Session::GetKeyframesMs() {
    std::lock_guard<std::mutex> lock(mutex_);
    return demuxer_->KeyframesMs();
}

std::vector<uint8_t> Session::GenerateInitSegment() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (init_built_ && !init_cache_.empty()) return init_cache_;

    std::vector<Fmp4Writer::Track> tracks;

    AVStream* vid = demuxer_->video_stream();
    if (vid) {
        Fmp4Writer::Track t{};
        t.track_id = 1;
        t.timescale = kVideoOutTb;
        t.codecpar  = vid->codecpar;
        AVDictionaryEntry* lang = av_dict_get(vid->metadata, "language", nullptr, 0);
        if (lang && lang->value && *lang->value) t.language = lang->value;
        tracks.push_back(t);
    }

    if (aac_ && aac_codecpar_) {
        Fmp4Writer::Track t{};
        t.track_id = 2;
        t.timescale = kAudioOutTb;
        t.codecpar = aac_codecpar_;

        AVStream* aud = demuxer_->audio_stream();
        if (aud) {
            AVDictionaryEntry* lang = av_dict_get(aud->metadata, "language", nullptr, 0);
            if (lang && lang->value && *lang->value) t.language = lang->value;
        }

        t.initial_padding = aac_->initial_padding();
        tracks.push_back(t);
    }

    init_cache_ = Fmp4Writer::BuildInit(tracks);
    init_built_ = true;
    std::cout << "[Session] init.mp4 ready (" << init_cache_.size() << " bytes, "
              << tracks.size() << " tracks)" << std::endl;
    return init_cache_;
}

std::vector<uint8_t> Session::GenerateSegment(int64_t segment_idx,
                                              int64_t segment_start_ms,
                                              int64_t segment_end_ms) {
    if (!init_built_) (void)GenerateInitSegment();
    std::lock_guard<std::mutex> lock(mutex_);
    return BuildSegmentLocked(segment_idx, segment_start_ms, segment_end_ms);
}

// ── Per-segment workspace ────────────────────────────────────────────────────
//
// Collect raw byte buffers for each video sample (we copy AVPacket data so the
// Sample::data pointers stay valid through BuildSegment) and AAC encoder
// outputs. Storage and Samples are kept side-by-side so the indices align.
struct SampleStore {
    std::vector<std::vector<uint8_t>> storage;
    std::vector<Sample> samples;

    void Append(const uint8_t* data, uint32_t size,
                int64_t dts, uint32_t duration, int32_t cts_offset, bool is_keyframe) {
        storage.emplace_back(data, data + size);
        Sample s{};
        s.data = storage.back().data();
        s.size = static_cast<uint32_t>(storage.back().size());
        s.dts = dts;
        s.duration = duration;
        s.cts_offset = cts_offset;
        s.is_keyframe = is_keyframe;
        samples.push_back(s);
    }

    void Append(const std::vector<uint8_t>& data,
                int64_t dts, uint32_t duration, int32_t cts_offset, bool is_keyframe) {
        storage.push_back(data);
        Sample s{};
        s.data = storage.back().data();
        s.size = static_cast<uint32_t>(storage.back().size());
        s.dts = dts;
        s.duration = duration;
        s.cts_offset = cts_offset;
        s.is_keyframe = is_keyframe;
        samples.push_back(s);
    }
};

std::vector<uint8_t> Session::BuildSegmentLocked(int64_t segment_idx,
                                                 int64_t segment_start_ms,
                                                 int64_t segment_end_ms) {
    auto cache_it = segment_cache_.find(segment_idx);
    if (cache_it != segment_cache_.end()) {
        std::cout << "[Session] Returning cached segment " << segment_idx
                  << " (" << cache_it->second.size() << " bytes)" << std::endl;
        return cache_it->second;
    }

    int64_t audio_decode_start_ms = std::max<int64_t>(0, segment_start_ms - kAudioPrerollMs);
    int64_t audio_encode_start_pts = MsToAudioPts(audio_decode_start_ms);
    audio_encode_start_pts = AudioFrameIndexForPts(audio_encode_start_pts) * 1024;

    int64_t segment_start_pts_48k = MsToAudioPts(segment_start_ms);
    int64_t segment_end_pts_48k = MsToAudioPts(segment_end_ms);
    int64_t output_start_frame =
        segment_idx == 0 && aac_ ? AudioFrameIndexForPts(-aac_->initial_padding())
                                 : AudioFrameIndexForPts(segment_start_pts_48k);
    int64_t output_end_frame = AudioFrameIndexForPts(segment_end_pts_48k);
    if (output_end_frame <= output_start_frame) output_end_frame = output_start_frame + 1;

    int64_t audio_cutoff_pts =
        MsToAudioPts(segment_end_ms + kAudioLookaheadMs);

    demuxer_->SeekToMs(audio_decode_start_ms);
    if (aac_) aac_->Reset(segment_idx == 0 ? -aac_->initial_padding()
                                           : audio_encode_start_pts);

    SampleStore video_store;
    SampleStore audio_store;

    int video_idx = demuxer_->video_stream_index();
    int audio_idx = demuxer_->audio_stream_index();
    AVStream* vid_stream = demuxer_->video_stream();
    AVStream* aud_stream = demuxer_->audio_stream();

    AVPacket* pkt = av_packet_alloc();
    bool seen_keyframe = false;
    bool hit_boundary = false;

    auto append_video_packet = [&](AVPacket* p) {
        if (!vid_stream) return;
        // Subtract stream start_time from real timestamps.
        if (vid_stream->start_time != AV_NOPTS_VALUE && vid_stream->start_time > 0) {
            if (p->dts != AV_NOPTS_VALUE) p->dts -= vid_stream->start_time;
            if (p->pts != AV_NOPTS_VALUE) p->pts -= vid_stream->start_time;
        }
        if (p->dts == AV_NOPTS_VALUE && p->pts != AV_NOPTS_VALUE) p->dts = p->pts;
        if (p->dts == AV_NOPTS_VALUE) return;
        if (p->pts == AV_NOPTS_VALUE) p->pts = p->dts;

        int64_t dts_out = av_rescale_q(p->dts, vid_stream->time_base, kVideoOutTb);
        int64_t pts_out = av_rescale_q(p->pts, vid_stream->time_base, kVideoOutTb);
        int32_t cts_off = static_cast<int32_t>(pts_out - dts_out);
        if (cts_off < 0) cts_off = 0;

        uint32_t duration = 0;
        if (p->duration > 0) {
            duration = static_cast<uint32_t>(av_rescale_q(p->duration, vid_stream->time_base, kVideoOutTb));
        }

        bool is_keyframe = (p->flags & AV_PKT_FLAG_KEY) != 0;

        video_store.Append(p->data, static_cast<uint32_t>(p->size),
                           dts_out, duration, cts_off, is_keyframe);
    };

    auto fix_video_durations = [&]() {
        // Per-sample duration: gap between successive DTS values, with the
        // last sample taking the same duration as its predecessor (or a sane
        // fallback if only one sample).
        auto& samples = video_store.samples;
        if (samples.empty()) return;
        for (size_t i = 0; i + 1 < samples.size(); ++i) {
            int64_t gap = samples[i + 1].dts - samples[i].dts;
            if (gap > 0) samples[i].duration = static_cast<uint32_t>(gap);
        }
        if (samples.size() >= 2) {
            samples.back().duration = samples[samples.size() - 2].duration;
        } else if (samples.back().duration == 0) {
            // Single-sample segment fallback: 1/30 sec at 16k tb.
            samples.back().duration = 16000 / 30;
        }
    };

    auto cache_audio_pkts = [&](std::vector<AVPacket*>& enc_pkts) {
        for (AVPacket* ep : enc_pkts) {
            if (ep->pts != AV_NOPTS_VALUE && ep->size > 0) {
                int64_t frame_idx = AudioFrameIndexForPts(ep->pts);
                audio_frame_cache_.try_emplace(
                    frame_idx,
                    std::vector<uint8_t>(ep->data, ep->data + ep->size));
            }
            av_packet_free(&ep);
        }
        enc_pkts.clear();
    };

    auto drain_aac = [&](int64_t cutoff_pts_48k) -> int {
        if (!aac_) return 0;
        std::vector<AVPacket*> enc_pkts;
        int n = aac_->EncodeUntil(cutoff_pts_48k, enc_pkts);
        cache_audio_pkts(enc_pkts);
        return n;
    };

    int total_audio_frames = 0;

    while (demuxer_->ReadPacket(pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            int64_t pkt_ms = av_rescale_q(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts,
                                          vid_stream->time_base, AVRational{1, 1000});
            if (!seen_keyframe) {
                if (pkt->flags & AV_PKT_FLAG_KEY) {
                    if (pkt_ms < segment_start_ms) {
                        av_packet_unref(pkt);
                        continue;
                    }
                    seen_keyframe = true;
                } else {
                    av_packet_unref(pkt);
                    continue;
                }
            }

            // Boundary: keyframe at/after segment_end belongs to the next segment.
            if (seen_keyframe && (pkt->flags & AV_PKT_FLAG_KEY) && pkt_ms >= segment_end_ms) {
                hit_boundary = true;
                av_packet_unref(pkt);

                if (aac_ && audio_idx >= 0 && aud_stream) {
                    // Drain audio packets past the boundary so we have enough
                    // samples to encode through the cutoff.
                    while (demuxer_->ReadPacket(pkt) >= 0) {
                        if (pkt->stream_index != audio_idx) {
                            av_packet_unref(pkt);
                            continue;
                        }
                        int64_t apkt_ms = pkt->pts != AV_NOPTS_VALUE
                            ? av_rescale_q(pkt->pts, aud_stream->time_base, AVRational{1, 1000})
                            : audio_decode_start_ms;
                        if (apkt_ms > segment_end_ms + kAudioLookaheadMs) {
                            av_packet_unref(pkt);
                            break;
                        }
                        if (apkt_ms >= audio_decode_start_ms) aac_->Decode(pkt);
                        av_packet_unref(pkt);
                    }
                }

                if (aac_) total_audio_frames += drain_aac(audio_cutoff_pts);
                break;
            }

            append_video_packet(pkt);
        } else if (pkt->stream_index == audio_idx && aac_) {
            int64_t apkt_ms = aud_stream && pkt->pts != AV_NOPTS_VALUE
                ? av_rescale_q(pkt->pts, aud_stream->time_base, AVRational{1, 1000})
                : audio_decode_start_ms;
            if (apkt_ms >= audio_decode_start_ms) aac_->Decode(pkt);
            total_audio_frames += drain_aac(audio_cutoff_pts);
        }

        av_packet_unref(pkt);
    }

    // EOF before boundary keyframe: encode whatever remains.
    if (aac_ && !hit_boundary) {
        total_audio_frames += drain_aac(audio_cutoff_pts);
    }

    av_packet_free(&pkt);
    fix_video_durations();

    if (aac_) {
        for (int64_t frame_idx = output_start_frame;
             frame_idx < output_end_frame;
             ++frame_idx) {
            auto it = audio_frame_cache_.find(frame_idx);
            if (it == audio_frame_cache_.end()) continue;
            audio_store.Append(it->second,
                               frame_idx * 1024,
                               1024,
                               /*cts_offset=*/0,
                               /*is_keyframe=*/true);
        }

        while (audio_frame_cache_.size() > kMaxCachedAudioFrames) {
            int64_t oldest = 0;
            bool found = false;
            for (const auto& [idx, _] : audio_frame_cache_) {
                if (!found || idx < oldest) {
                    oldest = idx;
                    found = true;
                }
            }
            if (!found) break;
            audio_frame_cache_.erase(oldest);
        }
    }

    if (video_store.samples.empty()) {
        throw std::runtime_error("Session::BuildSegmentLocked: no video samples for segment "
                                 + std::to_string(segment_idx));
    }

    // ── Build tracks + tfdt arrays in stable order ───────────────────────────
    std::vector<Fmp4Writer::Track> tracks;
    std::vector<std::vector<Sample>> samples_per_track;
    std::vector<int64_t> tfdt_per_track;

    {
        Fmp4Writer::Track t{};
        t.track_id = 1;
        t.timescale = kVideoOutTb;
        t.codecpar = vid_stream->codecpar;
        tracks.push_back(t);
        samples_per_track.push_back(std::move(video_store.samples));
        tfdt_per_track.push_back(samples_per_track.back().front().dts);
    }
    if (aac_ && aac_codecpar_ && !audio_store.samples.empty()) {
        Fmp4Writer::Track t{};
        t.track_id = 2;
        t.timescale = kAudioOutTb;
        t.codecpar = aac_codecpar_;
        tracks.push_back(t);
        samples_per_track.push_back(std::move(audio_store.samples));
        tfdt_per_track.push_back(samples_per_track.back().front().dts);
    }

    auto bytes = Fmp4Writer::BuildSegment(
        /*sequence_number=*/static_cast<uint32_t>(segment_idx + 1),
        tracks, samples_per_track, tfdt_per_track);

    // Track storage owned by the SampleStore vectors goes out of scope here,
    // but BuildSegment has already copied all bytes into the output buffer.

    if (total_audio_frames > 0) {
        std::cout << "[Session] Segment " << segment_idx << " complete: "
                  << samples_per_track[0].size() << " video samples, "
                  << total_audio_frames << " audio frames, "
                  << bytes.size() << " bytes" << std::endl;
    } else {
        std::cout << "[Session] Segment " << segment_idx << " complete (video-only): "
                  << samples_per_track[0].size() << " samples, "
                  << bytes.size() << " bytes" << std::endl;
    }

    segment_cache_[segment_idx] = bytes;

    // Evict oldest entries past kMaxCachedSegments.
    while (segment_cache_.size() > kMaxCachedSegments) {
        int64_t oldest = segment_idx;
        for (const auto& [idx, _] : segment_cache_) if (idx < oldest) oldest = idx;
        if (oldest == segment_idx) break;
        segment_cache_.erase(oldest);
    }

    return segment_cache_[segment_idx];
}

}  // namespace fmp4
