#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

#include "Sample.h"

namespace fmp4 {

// Pure-function fMP4 (CMAF) writer.
//
// Every segment's bytes are a function of (sequence_number, tracks, samples,
// tfdt) — there is NO cross-call state. This is the entire point of the
// rewrite: it removes the muxer-state juggling and box-patching that the
// FFmpeg mp4 muxer required.
//
// Track 0 is treated as "video", track 1 as "audio". (We rely on the order
// for sidx/traf emission but otherwise the writer is codec-agnostic so far
// as the boxes are concerned.)
class Fmp4Writer {
public:
    struct Track {
        uint32_t track_id;                 // 1 for video, 2 for audio (== mp4 track number)
        AVRational timescale;              // {1, 16000} video, {1, 48000} audio
        const AVCodecParameters* codecpar; // for avcC / hvcC / esds extradata + width/height/etc.
        std::optional<std::string> language;
        // For audio tracks, the AAC encoder's `initial_padding` (priming
        // samples). Used to write an `edts/elst` with `media_time =
        // initial_padding` so playback skips the priming silence.
        // For video this is typically 0 (or a small number reflecting CTS
        // delay) and should be left at 0 for keyframe-anchored video edits.
        int64_t initial_padding = 0;
    };

    // ── Init segment ───────────────────────────────────────────────────────
    //
    // Emits ftyp + moov for the supplied track set. The output is a complete
    // CMAF init segment that hls.js / Safari will accept as `EXT-X-MAP:URI`.
    //
    // Throws std::runtime_error on malformed inputs.
    static std::vector<uint8_t> BuildInit(const std::vector<Track>& tracks);

    // ── Media segment ──────────────────────────────────────────────────────
    //
    // Emits styp + sidx (one per track) + moof + mdat. Track sample order in
    // `samples_per_track` and `tfdt_per_track` MUST match `tracks`. `samples`
    // for an empty track may itself be empty (e.g. very short audio span).
    //
    // `tfdt_per_track[i]` is the `base_media_decode_time` for track i in its
    // own timescale. Callers compute this from the segment-start timestamp
    // alone (e.g. `av_rescale_q(segment_start_ms, {1,1000}, track.timescale)`
    // for audio; video tfdt may include a reorder-delay subtraction).
    static std::vector<uint8_t> BuildSegment(
        uint32_t sequence_number,
        const std::vector<Track>& tracks,
        const std::vector<std::vector<Sample>>& samples_per_track,
        const std::vector<int64_t>& tfdt_per_track);
};

}  // namespace fmp4
