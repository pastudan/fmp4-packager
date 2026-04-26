#include "AacTranscoder.h"

#include <stdexcept>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace fmp4 {

AacTranscoder::AacTranscoder(AVCodecParameters* in_codecpar) {
    if (!in_codecpar) throw std::runtime_error("AacTranscoder: null codecpar");

    const AVCodec* decoder = avcodec_find_decoder(in_codecpar->codec_id);
    if (!decoder) throw std::runtime_error("AacTranscoder: source decoder not found");

    dec_ctx_ = avcodec_alloc_context3(decoder);
    if (!dec_ctx_) throw std::runtime_error("AacTranscoder: alloc decoder ctx failed");

    avcodec_parameters_to_context(dec_ctx_, in_codecpar);

    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (dec_ctx_->ch_layout.nb_channels == 0) {
        av_channel_layout_copy(&dec_ctx_->ch_layout, &stereo);
    }

    if (avcodec_open2(dec_ctx_, decoder, nullptr) < 0) {
        throw std::runtime_error("AacTranscoder: avcodec_open2(decoder) failed");
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!encoder) throw std::runtime_error("AacTranscoder: AAC encoder not found");

    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) throw std::runtime_error("AacTranscoder: alloc encoder ctx failed");

    enc_ctx_->sample_rate = 48000;
    av_channel_layout_copy(&enc_ctx_->ch_layout, &stereo);
    // AAC encoder always wants planar float input; hardcoding avoids the
    // deprecated AVCodec::sample_fmts accessor on newer FFmpeg.
    enc_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc_ctx_->bit_rate   = 128000;
    enc_ctx_->time_base  = AVRational{1, 48000};

    if (avcodec_open2(enc_ctx_, encoder, nullptr) < 0) {
        throw std::runtime_error("AacTranscoder: avcodec_open2(encoder) failed");
    }

    initial_padding_ = enc_ctx_->initial_padding > 0 ? enc_ctx_->initial_padding : 1024;

    if (swr_alloc_set_opts2(&swr_ctx_,
            &enc_ctx_->ch_layout, AV_SAMPLE_FMT_FLTP, 48000,
            &dec_ctx_->ch_layout, dec_ctx_->sample_fmt, dec_ctx_->sample_rate,
            0, nullptr) < 0) {
        throw std::runtime_error("AacTranscoder: swr_alloc_set_opts2 failed");
    }
    if (swr_init(swr_ctx_) < 0) {
        throw std::runtime_error("AacTranscoder: swr_init failed");
    }
}

AacTranscoder::~AacTranscoder() {
    if (swr_ctx_) swr_free(&swr_ctx_);
    if (enc_ctx_) avcodec_free_context(&enc_ctx_);
    if (dec_ctx_) avcodec_free_context(&dec_ctx_);
}

void AacTranscoder::Reset(int64_t next_pts_48k) {
    left_queue_.clear();
    right_queue_.clear();
    audio_pts_ = next_pts_48k;
    if (dec_ctx_) avcodec_flush_buffers(dec_ctx_);
    if (enc_ctx_) avcodec_flush_buffers(enc_ctx_);
    if (swr_ctx_) {
        swr_close(swr_ctx_);
        swr_init(swr_ctx_);
    }
}

void AacTranscoder::Decode(AVPacket* in_pkt) {
    if (!dec_ctx_ || !swr_ctx_) return;

    AVFrame* frame     = av_frame_alloc();
    AVFrame* resampled = av_frame_alloc();
    if (!frame || !resampled) {
        if (frame) av_frame_free(&frame);
        if (resampled) av_frame_free(&resampled);
        return;
    }

    avcodec_send_packet(dec_ctx_, in_pkt);
    while (avcodec_receive_frame(dec_ctx_, frame) == 0) {
        resampled->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_copy(&resampled->ch_layout, &enc_ctx_->ch_layout);
        resampled->sample_rate = 48000;
        resampled->nb_samples  = swr_get_out_samples(swr_ctx_, frame->nb_samples);
        av_frame_get_buffer(resampled, 0);

        int samples = swr_convert(swr_ctx_,
            resampled->data, resampled->nb_samples,
            (const uint8_t**)frame->data, frame->nb_samples);

        if (samples > 0) {
            float* left  = reinterpret_cast<float*>(resampled->data[0]);
            float* right = reinterpret_cast<float*>(resampled->data[1]);
            left_queue_.insert(left_queue_.end(), left, left + samples);
            right_queue_.insert(right_queue_.end(), right, right + samples);
        }
        av_frame_unref(resampled);
        av_frame_unref(frame);
    }

    av_frame_free(&resampled);
    av_frame_free(&frame);
}

int AacTranscoder::EncodeUntil(int64_t cutoff_pts_48k, std::vector<AVPacket*>& out) {
    int frames_encoded = 0;
    while (left_queue_.size() >= 1024 && right_queue_.size() >= 1024) {
        if (audio_pts_ + 1024 > cutoff_pts_48k) break;

        AVFrame* aac_frame = av_frame_alloc();
        if (!aac_frame) break;
        aac_frame->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_copy(&aac_frame->ch_layout, &enc_ctx_->ch_layout);
        aac_frame->sample_rate = 48000;
        aac_frame->nb_samples  = 1024;
        av_frame_get_buffer(aac_frame, 0);

        std::copy_n(left_queue_.begin(),  1024, reinterpret_cast<float*>(aac_frame->data[0]));
        std::copy_n(right_queue_.begin(), 1024, reinterpret_cast<float*>(aac_frame->data[1]));
        left_queue_.erase(left_queue_.begin(),  left_queue_.begin() + 1024);
        right_queue_.erase(right_queue_.begin(), right_queue_.begin() + 1024);

        aac_frame->pts = audio_pts_;

        avcodec_send_frame(enc_ctx_, aac_frame);
        av_frame_free(&aac_frame);

        AVPacket* pkt = av_packet_alloc();
        while (pkt && avcodec_receive_packet(enc_ctx_, pkt) == 0) {
            if (pkt->pts == AV_NOPTS_VALUE) pkt->pts = audio_pts_;
            if (pkt->dts == AV_NOPTS_VALUE) pkt->dts = pkt->pts;
            pkt->duration = 1024;
            pkt->time_base = AVRational{1, 48000};
            out.push_back(pkt);
            pkt = av_packet_alloc();  // next iteration
        }
        if (pkt) av_packet_free(&pkt);  // unused alloc

        audio_pts_ += 1024;
        ++frames_encoded;
    }
    return frames_encoded;
}

}  // namespace fmp4
