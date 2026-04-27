#include "Session.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
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
                 int file_index,
                 int session_id)
    : url_(webtorrent_url + "/files/" + infohash + "/" + std::to_string(file_index))
    , short_infohash_(infohash.substr(0, std::min<size_t>(8, infohash.size())))
    , session_id_(session_id)
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
    auto t0 = std::chrono::steady_clock::now();

    // Header line (no indent), printed once for every segment request.
    {
        std::ostringstream hdr;
        hdr << short_infohash_ << " s" << session_id_ << " #" << segment_idx << "\n";
        std::cout << hdr.str() << std::flush;
    }

    auto cache_it = segment_cache_.find(segment_idx);
    if (cache_it != segment_cache_.end()) {
        std::ostringstream dbg;
        dbg << "    [cache hit] returning cached segment ("
            << cache_it->second.size() << " bytes)\n";
        std::cout << dbg.str() << std::flush;
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

    // Snapshot the AAC frame cache before this build so we can report how many
    // of the requested output frames were satisfied from cache vs newly encoded.
    int audio_cache_hits_pre = 0;
    if (aac_) {
        for (int64_t i = output_start_frame; i < output_end_frame; ++i) {
            if (audio_frame_cache_.count(i)) ++audio_cache_hits_pre;
        }
    }

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

    // ── Debug counters ────────────────────────────────────────────────────────
    int audio_packets_decoded = 0;
    int64_t src_audio_pts_first_ms = INT64_MAX;
    int64_t src_audio_pts_last_ms = INT64_MIN;
    int64_t src_audio_pts_first_native = INT64_MAX;
    int64_t src_audio_pts_last_native = INT64_MIN;
    int video_keyframe_count = 0;

    auto note_audio_pkt = [&](AVPacket* ap) {
        ++audio_packets_decoded;
        if (aud_stream && ap->pts != AV_NOPTS_VALUE) {
            int64_t ms = av_rescale_q(ap->pts, aud_stream->time_base, AVRational{1, 1000});
            if (ms < src_audio_pts_first_ms) src_audio_pts_first_ms = ms;
            if (ms > src_audio_pts_last_ms)  src_audio_pts_last_ms  = ms;
            if (ap->pts < src_audio_pts_first_native) src_audio_pts_first_native = ap->pts;
            if (ap->pts > src_audio_pts_last_native)  src_audio_pts_last_native  = ap->pts;
        }
    };

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
        if (is_keyframe) ++video_keyframe_count;

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
                        if (apkt_ms >= audio_decode_start_ms) {
                            note_audio_pkt(pkt);
                            aac_->Decode(pkt);
                        }
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
            if (apkt_ms >= audio_decode_start_ms) {
                note_audio_pkt(pkt);
                aac_->Decode(pkt);
            }
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

    // ── Debug block ──────────────────────────────────────────────────────────
    {
        const auto& vsamples = samples_per_track[0];
        int64_t v_dts_first = vsamples.front().dts;
        int64_t v_dts_last  = vsamples.back().dts;
        int64_t v_pts_first = INT64_MAX, v_pts_last = INT64_MIN;
        int32_t cts_min = INT32_MAX, cts_max = INT32_MIN;
        uint32_t dur_min = UINT32_MAX, dur_max = 0;
        uint64_t dur_sum = 0;
        for (const auto& s : vsamples) {
            int64_t pts = s.dts + s.cts_offset;
            if (pts < v_pts_first) v_pts_first = pts;
            if (pts > v_pts_last)  v_pts_last  = pts;
            if (s.cts_offset < cts_min) cts_min = s.cts_offset;
            if (s.cts_offset > cts_max) cts_max = s.cts_offset;
            if (s.duration < dur_min) dur_min = s.duration;
            if (s.duration > dur_max) dur_max = s.duration;
            dur_sum += s.duration;
        }
        uint32_t dur_avg = vsamples.empty() ? 0
            : static_cast<uint32_t>(dur_sum / vsamples.size());
        int64_t v_dts_first_ms = av_rescale_q(v_dts_first, kVideoOutTb, AVRational{1, 1000});
        int64_t v_dts_last_ms  = av_rescale_q(v_dts_last,  kVideoOutTb, AVRational{1, 1000});
        int64_t v_pts_first_ms = av_rescale_q(v_pts_first, kVideoOutTb, AVRational{1, 1000});
        int64_t v_pts_last_ms  = av_rescale_q(v_pts_last,  kVideoOutTb, AVRational{1, 1000});
        int64_t v_tfdt = tfdt_per_track[0];

        // Audio output stats from the audio_store before it was moved.
        bool have_audio = aac_ && aac_codecpar_ && samples_per_track.size() > 1;
        int64_t a_tfdt = have_audio ? tfdt_per_track[1] : 0;
        size_t a_count = have_audio ? samples_per_track[1].size() : 0;
        int64_t a_pts_first = 0, a_pts_last = 0;
        int64_t a_idx_first = 0, a_idx_last = 0;
        if (have_audio && a_count > 0) {
            const auto& as = samples_per_track[1];
            a_pts_first = as.front().dts;
            a_pts_last  = as.back().dts;
            a_idx_first = AudioFrameIndexForPts(a_pts_first);
            a_idx_last  = AudioFrameIndexForPts(a_pts_last);
        }

        // Cache window after this build.
        int64_t cache_idx_min = INT64_MAX, cache_idx_max = INT64_MIN;
        for (const auto& [idx, _] : audio_frame_cache_) {
            if (idx < cache_idx_min) cache_idx_min = idx;
            if (idx > cache_idx_max) cache_idx_max = idx;
        }

        // Missing frame indices in the requested output range.
        std::vector<int64_t> missing;
        if (aac_) {
            for (int64_t i = output_start_frame; i < output_end_frame; ++i) {
                if (!audio_frame_cache_.count(i)) missing.push_back(i);
            }
        }

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        std::ostringstream dbg;
        dbg << "    range:    [" << segment_start_ms << " ms, " << segment_end_ms
            << " ms]  span=" << (segment_end_ms - segment_start_ms) << " ms\n";
        dbg << "    video:    " << vsamples.size() << " samples ("
            << video_keyframe_count << " keyframes)  tb=1/16000\n";
        dbg << "      dts:    [" << v_dts_first << ", " << v_dts_last
            << "]  ms=[" << v_dts_first_ms << ", " << v_dts_last_ms << "]\n";
        dbg << "      pts:    [" << v_pts_first << ", " << v_pts_last
            << "]  ms=[" << v_pts_first_ms << ", " << v_pts_last_ms << "]\n";
        dbg << "      tfdt:   " << v_tfdt << " ticks\n";
        dbg << "      cts_off:[" << cts_min << ", " << cts_max
            << "]  durations: min=" << dur_min << " avg=" << dur_avg
            << " max=" << dur_max << "\n";

        if (aac_) {
            dbg << "    audio:    " << a_count << " AAC frames written  tb=1/48000\n";
            dbg << "      output_range: frame_idx=[" << output_start_frame
                << ", " << output_end_frame << ")  ("
                << (output_end_frame - output_start_frame) << " frames)\n";
            dbg << "      cache_hits_pre_build: " << audio_cache_hits_pre << "/"
                << (output_end_frame - output_start_frame) << "\n";
            dbg << "      encoded_this_build:   " << total_audio_frames
                << " AAC frames\n";
            dbg << "      src TrueHD pkts decoded: " << audio_packets_decoded;
            if (audio_packets_decoded > 0) {
                dbg << "  src_pts_ms=[" << src_audio_pts_first_ms
                    << ", " << src_audio_pts_last_ms << "]";
                if (aud_stream) {
                    dbg << "  src_pts_native=[" << src_audio_pts_first_native
                        << ", " << src_audio_pts_last_native
                        << "] tb=" << aud_stream->time_base.num << "/"
                        << aud_stream->time_base.den;
                }
            }
            dbg << "\n";
            dbg << "      decode_window: ms=[" << audio_decode_start_ms
                << ", " << (segment_end_ms + kAudioLookaheadMs)
                << "]  cutoff_pts_48k=" << audio_cutoff_pts << "\n";
            if (a_count > 0) {
                dbg << "      out frame_idx: [" << a_idx_first << ", "
                    << a_idx_last << "]  pts_48k=[" << a_pts_first << ", "
                    << a_pts_last << "]\n";
                dbg << "      audio tfdt:   " << a_tfdt << " ticks ("
                    << av_rescale_q(a_tfdt, kAudioOutTb, AVRational{1, 1000})
                    << " ms)\n";
            } else {
                dbg << "      out frame_idx: <none>\n";
            }
            dbg << "      cache: " << audio_frame_cache_.size() << " entries";
            if (cache_idx_min != INT64_MAX) {
                dbg << "  idx=[" << cache_idx_min << ", " << cache_idx_max << "]";
            }
            dbg << "\n";
            if (missing.empty()) {
                dbg << "      gaps in output range: none\n";
            } else {
                dbg << "      gaps in output range: " << missing.size()
                    << " missing frame(s):";
                size_t show = std::min<size_t>(missing.size(), 16);
                for (size_t i = 0; i < show; ++i) dbg << " " << missing[i];
                if (missing.size() > show) dbg << " ...";
                dbg << "\n";
            }
        } else {
            dbg << "    audio:    <no audio track>\n";
        }
        dbg << "    bytes:    " << bytes.size()
            << "  elapsed=" << elapsed_ms << " ms\n";
        std::cout << dbg.str() << std::flush;
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
