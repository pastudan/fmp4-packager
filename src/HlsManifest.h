#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace fmp4 {

// Generate an HLS VOD manifest (`#EXT-X-PLAYLIST-TYPE:VOD`) for the given
// keyframe boundaries. `keyframes_ms` is a sorted list of segment start times
// in milliseconds, with the last entry being the file's end time (i.e. the
// number of segments is `keyframes_ms.size() - 1`).
//
// `segment_prefix` is prepended to every URL emitted in the playlist and to
// the `#EXT-X-MAP:URI`. Typically `"<infohash>/session-<n>/"`.
class HlsManifest {
public:
    static std::string Generate(const std::vector<int64_t>& keyframes_ms,
                                const std::string& segment_prefix);

    // Per-segment durations in seconds, one entry per segment.
    static std::vector<double> SegmentDurations(const std::vector<int64_t>& keyframes_ms);
};

}  // namespace fmp4
