#pragma once

#include <cstdint>

namespace fmp4 {

// One coded sample (NAL access unit for video, AAC frame for audio) plus the
// timing information needed by `trun` and `sidx`.
//
// `data`/`size` are NOT owned by this struct — callers keep the bytes alive
// (via `std::vector<uint8_t>` storage in the segment-builder workspace) for
// as long as Fmp4Writer::BuildSegment is in scope.
struct Sample {
    const uint8_t* data;
    uint32_t size;
    int64_t  dts;          // in track timescale (e.g. 1/16000 video, 1/48000 audio)
    uint32_t duration;     // in track timescale
    int32_t  cts_offset;   // PTS - DTS, in track timescale (0 for audio)
    bool     is_keyframe;  // sets trun sample_flags non-sync to 0
};

}  // namespace fmp4
