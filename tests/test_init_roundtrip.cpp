// Black-box test: build an init segment from a synthetic AVCodecParameters
// and verify the output parses cleanly via libavformat (so an mp4 demuxer
// can read it back, which is the same kind of demuxing the browser does).
//
// We use a tiny fabricated avcC payload: the actual SPS/PPS bytes don't have
// to be valid for box-structure validation; libavformat reports the boxes
// as-is and only complains if the *outer* mp4 framing is malformed.
//
// Build with:  cmake -S . -B build -DBUILD_TESTS=ON
// Run    with: ./build/test_init_roundtrip

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

// Minimal AVCDecoderConfigurationRecord for an H.264 stream. The payload
// itself is enough to satisfy the avcC box parser even if the SPS/PPS are
// not parsed further.
const uint8_t kFakeAvcC[] = {
    0x01,                   // configurationVersion
    0x42,                   // AVCProfileIndication (Baseline)
    0x00,                   // profile_compatibility
    0x1F,                   // AVCLevelIndication
    0xFF,                   // 6 bits reserved + 2 bits lengthSizeMinusOne (3 = 4-byte)
    0xE1,                   // 3 bits reserved + 5 bits numOfSequenceParameterSets (1)
    0x00, 0x09,             // SPS length
    0x67, 0x42, 0x00, 0x1E, 0x99, 0x9A, 0x14, 0x01, 0x6E,   // SPS bytes
    0x01,                   // numOfPictureParameterSets (1)
    0x00, 0x04,             // PPS length
    0x68, 0xCE, 0x3C, 0x80  // PPS bytes
};

struct ReadBuffer {
    const std::vector<uint8_t>* data;
    int64_t pos;
};

int ReadCallback(void* opaque, uint8_t* buf, int buf_size) {
    auto* rb = static_cast<ReadBuffer*>(opaque);
    int64_t avail = static_cast<int64_t>(rb->data->size()) - rb->pos;
    if (avail <= 0) return AVERROR_EOF;
    int to_copy = static_cast<int>(std::min<int64_t>(avail, buf_size));
    std::memcpy(buf, rb->data->data() + rb->pos, to_copy);
    rb->pos += to_copy;
    return to_copy;
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

    // ── 1. Build init bytes ──────────────────────────────────────────────────
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

    std::vector<uint8_t> init = fmp4::Fmp4Writer::BuildInit({t});

    // ── 2. Save for ffprobe inspection ───────────────────────────────────────
    {
        std::ofstream f("test_init.mp4", std::ios::binary);
        f.write(reinterpret_cast<const char*>(init.data()), init.size());
    }
    std::fprintf(stderr, "test_init.mp4 written (%zu bytes)\n", init.size());

    // ── 3. Re-open through libavformat to validate box structure ─────────────
    ReadBuffer rb{ &init, 0 };
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
        std::fprintf(stderr, "FAIL: avformat_open_input failed: %s\n", err);
        return 1;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        char err[256];
        av_make_error_string(err, sizeof(err), ret);
        std::fprintf(stderr, "FAIL: avformat_find_stream_info failed: %s\n", err);
        return 1;
    }

    if (fmt_ctx->nb_streams != 1) {
        std::fprintf(stderr, "FAIL: expected 1 stream, got %u\n", fmt_ctx->nb_streams);
        return 1;
    }

    AVStream* s = fmt_ctx->streams[0];
    if (s->codecpar->codec_id != AV_CODEC_ID_H264) {
        std::fprintf(stderr, "FAIL: codec_id %d != H264\n", s->codecpar->codec_id);
        return 1;
    }
    if (s->codecpar->width != 1280 || s->codecpar->height != 720) {
        std::fprintf(stderr, "FAIL: dimensions %dx%d != 1280x720\n",
                     s->codecpar->width, s->codecpar->height);
        return 1;
    }

    std::fprintf(stderr, "Input format: %s\n", fmt_ctx->iformat->name);
    std::fprintf(stderr, "Stream 0: %s %dx%d, time_base=%d/%d\n",
                 avcodec_get_name(s->codecpar->codec_id),
                 s->codecpar->width, s->codecpar->height,
                 s->time_base.num, s->time_base.den);

    avformat_close_input(&fmt_ctx);
    av_freep(&avio_ctx->buffer);
    avio_context_free(&avio_ctx);
    avcodec_parameters_free(&par);

    std::printf("test_init_roundtrip: OK\n");
    return 0;
}
