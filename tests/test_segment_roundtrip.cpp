// Build an init.mp4 + 1 media segment from a fabricated AVC stream, then
// re-open the concatenated bytes through libavformat to verify the fragment
// boxes (styp/sidx/moof/mfhd/tfhd/tfdt/trun/mdat) all parse correctly and
// the demuxer can iterate the samples we wrote.
//
// The "AVC" payload is fake — we just write 4-byte length prefixes followed
// by arbitrary bytes — but libavformat's mp4 demuxer doesn't decode them, it
// only checks that mdat/trun/sample-sizes line up. That's exactly what we
// want to verify at this layer.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include "Fmp4Writer.h"

namespace {

const uint8_t kFakeAvcC[] = {
    0x01, 0x42, 0x00, 0x1F, 0xFF, 0xE1,
    0x00, 0x09, 0x67, 0x42, 0x00, 0x1E, 0x99, 0x9A, 0x14, 0x01, 0x6E,
    0x01, 0x00, 0x04, 0x68, 0xCE, 0x3C, 0x80
};

struct ReadBuffer {
    const std::vector<uint8_t>* data;
    int64_t pos;
};

int ReadCallback(void* opaque, uint8_t* buf, int buf_size) {
    auto* rb = static_cast<ReadBuffer*>(opaque);
    int64_t avail = static_cast<int64_t>(rb->data->size()) - rb->pos;
    if (avail <= 0) return AVERROR_EOF;
    int n = static_cast<int>(std::min<int64_t>(avail, buf_size));
    std::memcpy(buf, rb->data->data() + rb->pos, n);
    rb->pos += n;
    return n;
}

int64_t SeekCallback(void* opaque, int64_t offset, int whence) {
    auto* rb = static_cast<ReadBuffer*>(opaque);
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(rb->data->size());
    int64_t p = rb->pos;
    switch (whence) {
        case SEEK_SET: p = offset; break;
        case SEEK_CUR: p += offset; break;
        case SEEK_END: p = rb->data->size() + offset; break;
        default: return AVERROR(EINVAL);
    }
    if (p < 0 || p > static_cast<int64_t>(rb->data->size())) return AVERROR(EINVAL);
    rb->pos = p;
    return p;
}

}  // namespace

int main() {
    av_log_set_level(AV_LOG_FATAL);

    // ── 1. Build init.mp4 ────────────────────────────────────────────────────
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
    t.timescale = AVRational{1, 16000};   // 16 kHz video tb (matches Session's tb)
    t.codecpar = par;
    t.language = "und";

    std::vector<uint8_t> init = fmp4::Fmp4Writer::BuildInit({t});

    // ── 2. Build a segment with 25 fake samples @ 25 fps (640 ticks each at 16k) ──
    // 25 fps means 1/25 sec = 16000/25 = 640 ticks per frame. A 1-second
    // segment has 25 samples.
    constexpr int kSampleCount = 25;
    constexpr uint32_t kSampleDuration = 640;

    // Allocate sample payload buffers (we keep them alive until BuildSegment
    // returns; Sample::data is non-owning).
    std::vector<std::vector<uint8_t>> sample_storage;
    sample_storage.reserve(kSampleCount);

    std::vector<fmp4::Sample> samples;
    samples.reserve(kSampleCount);

    int64_t dts = 0;
    for (int i = 0; i < kSampleCount; ++i) {
        // 4-byte length prefix + 60 fake NAL bytes = 64 bytes total.
        sample_storage.emplace_back(64);
        auto& payload = sample_storage.back();
        // Length prefix: 60 bytes of NAL.
        payload[0] = 0x00; payload[1] = 0x00; payload[2] = 0x00; payload[3] = 60;
        for (size_t j = 4; j < payload.size(); ++j) {
            payload[j] = static_cast<uint8_t>(j & 0xFF);
        }

        fmp4::Sample s{};
        s.data = payload.data();
        s.size = static_cast<uint32_t>(payload.size());
        s.dts = dts;
        s.duration = kSampleDuration;
        s.cts_offset = 0;
        s.is_keyframe = (i == 0);
        samples.push_back(s);
        dts += kSampleDuration;
    }

    std::vector<uint8_t> seg = fmp4::Fmp4Writer::BuildSegment(
        /*sequence_number=*/1,
        /*tracks=*/{t},
        /*samples_per_track=*/{samples},
        /*tfdt_per_track=*/{0});

    // ── 3. Concatenate init+seg and dump to disk for ffprobe inspection ─────
    std::vector<uint8_t> combined;
    combined.reserve(init.size() + seg.size());
    combined.insert(combined.end(), init.begin(), init.end());
    combined.insert(combined.end(), seg.begin(), seg.end());

    {
        std::ofstream f("test_seg.mp4", std::ios::binary);
        f.write(reinterpret_cast<const char*>(combined.data()), combined.size());
    }
    std::fprintf(stderr, "test_seg.mp4 written: init=%zu + seg=%zu = %zu bytes\n",
                 init.size(), seg.size(), combined.size());

    // ── 4. Re-open through libavformat to validate ──────────────────────────
    ReadBuffer rb{ &combined, 0 };
    constexpr int kAvioBufferSize = 4096;
    auto* avio_buffer = static_cast<uint8_t*>(av_malloc(kAvioBufferSize));
    AVIOContext* avio_ctx = avio_alloc_context(
        avio_buffer, kAvioBufferSize, 0, &rb, ReadCallback, nullptr, SeekCallback);

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    fmt_ctx->pb = avio_ctx;

    int ret = avformat_open_input(&fmt_ctx, nullptr, nullptr, nullptr);
    if (ret < 0) {
        char err[256];
        av_make_error_string(err, sizeof(err), ret);
        std::fprintf(stderr, "FAIL: avformat_open_input: %s\n", err);
        return 1;
    }
    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        char err[256];
        av_make_error_string(err, sizeof(err), ret);
        std::fprintf(stderr, "FAIL: avformat_find_stream_info: %s\n", err);
        return 1;
    }

    int packets_seen = 0;
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == 0) packets_seen++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    avformat_close_input(&fmt_ctx);
    av_freep(&avio_ctx->buffer);
    avio_context_free(&avio_ctx);
    avcodec_parameters_free(&par);

    if (packets_seen != kSampleCount) {
        std::fprintf(stderr, "FAIL: read %d packets, expected %d\n",
                     packets_seen, kSampleCount);
        return 1;
    }

    std::printf("test_segment_roundtrip: OK (%d packets)\n", packets_seen);
    return 0;
}
