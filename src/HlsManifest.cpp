#include "HlsManifest.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace fmp4 {

std::vector<double> HlsManifest::SegmentDurations(const std::vector<int64_t>& keyframes_ms) {
    std::vector<double> durations;
    if (keyframes_ms.size() < 2) return durations;

    durations.reserve(keyframes_ms.size() - 1);
    for (size_t i = 1; i < keyframes_ms.size(); ++i) {
        double dur = (keyframes_ms[i] - keyframes_ms[i - 1]) / 1000.0;
        durations.push_back(dur);
    }
    return durations;
}

std::string HlsManifest::Generate(const std::vector<int64_t>& keyframes_ms,
                                  const std::string& segment_prefix) {
    if (keyframes_ms.size() < 2) {
        return "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-ENDLIST\n";
    }

    auto durations = SegmentDurations(keyframes_ms);
    double max_duration = *std::max_element(durations.begin(), durations.end());
    int target_duration = static_cast<int>(std::ceil(max_duration));
    if (target_duration < 1) target_duration = 1;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6);

    ss << "#EXTM3U\n";
    ss << "#EXT-X-VERSION:7\n";
    ss << "#EXT-X-TARGETDURATION:" << target_duration << "\n";
    ss << "#EXT-X-MEDIA-SEQUENCE:0\n";
    ss << "#EXT-X-PLAYLIST-TYPE:VOD\n";
    // Intentionally omit EXT-X-INDEPENDENT-SEGMENTS; matches hlsenc-fork's
    // findings (segments may have leading non-key frames in audio).
    ss << "#EXT-X-MAP:URI=\"" << segment_prefix << "init.mp4\"\n";

    for (size_t i = 0; i < durations.size(); ++i) {
        ss << "#EXTINF:" << durations[i] << ",\n";
        ss << segment_prefix << i << ".m4s\n";
    }

    ss << "#EXT-X-ENDLIST\n";
    return ss.str();
}

}  // namespace fmp4
