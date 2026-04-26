// Verify Fmp4Writer::BuildSegment is a pure function: given identical inputs,
// the output bytes are byte-identical. This is the central invariant of the
// rewrite — re-fetching segment N from a fresh process MUST return the same
// bytes that were served the first time.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}

#include "Fmp4Writer.h"

namespace {

const uint8_t kFakeAvcC[] = {
    0x01, 0x42, 0x00, 0x1F, 0xFF, 0xE1,
    0x00, 0x09, 0x67, 0x42, 0x00, 0x1E, 0x99, 0x9A, 0x14, 0x01, 0x6E,
    0x01, 0x00, 0x04, 0x68, 0xCE, 0x3C, 0x80
};

std::vector<uint8_t> BuildOnce(uint32_t seq) {
    AVCodecParameters* par = avcodec_parameters_alloc();
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id   = AV_CODEC_ID_H264;
    par->width      = 1280;
    par->height     = 720;
    par->extradata_size = sizeof(kFakeAvcC);
    par->extradata = static_cast<uint8_t*>(av_malloc(par->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(par->extradata, kFakeAvcC, par->extradata_size);
    std::memset(par->extradata + par->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    fmp4::Fmp4Writer::Track t{};
    t.track_id = 1;
    t.timescale = AVRational{1, 16000};
    t.codecpar = par;
    t.language = "und";

    constexpr int N = 30;
    std::vector<std::vector<uint8_t>> storage;
    storage.reserve(N);
    std::vector<fmp4::Sample> samples;
    samples.reserve(N);
    int64_t dts = 0;
    for (int i = 0; i < N; ++i) {
        storage.emplace_back(80);
        auto& payload = storage.back();
        // 4-byte length prefix + 76 fake NAL bytes.
        payload[0] = 0x00; payload[1] = 0x00; payload[2] = 0x00; payload[3] = 76;
        for (size_t j = 4; j < payload.size(); ++j) {
            payload[j] = static_cast<uint8_t>((i * 31 + j) & 0xFF);
        }
        fmp4::Sample s{};
        s.data = payload.data();
        s.size = static_cast<uint32_t>(payload.size());
        s.dts = dts;
        s.duration = 533;  // 30 fps at 16000 ≈ 533.33
        s.cts_offset = 0;
        s.is_keyframe = (i == 0);
        samples.push_back(s);
        dts += 533;
    }

    auto bytes = fmp4::Fmp4Writer::BuildSegment(seq, {t}, {samples}, {0});
    avcodec_parameters_free(&par);
    return bytes;
}

}  // namespace

int main() {
    av_log_set_level(AV_LOG_FATAL);

    auto a = BuildOnce(/*seq=*/42);
    auto b = BuildOnce(/*seq=*/42);

    if (a.size() != b.size()) {
        std::fprintf(stderr, "FAIL: size differs (a=%zu vs b=%zu)\n", a.size(), b.size());
        return 1;
    }
    if (std::memcmp(a.data(), b.data(), a.size()) != 0) {
        // Find first divergence for debugging.
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) {
                std::fprintf(stderr, "FAIL: byte %zu: a=0x%02X b=0x%02X\n", i, a[i], b[i]);
                break;
            }
        }
        return 1;
    }

    // Sanity: changing the sequence number must produce different bytes.
    auto c = BuildOnce(/*seq=*/43);
    if (a.size() == c.size() && std::memcmp(a.data(), c.data(), a.size()) == 0) {
        std::fprintf(stderr, "FAIL: different seq produced identical output\n");
        return 1;
    }

    std::printf("test_determinism: OK (%zu bytes, identical across two builds)\n", a.size());
    return 0;
}
