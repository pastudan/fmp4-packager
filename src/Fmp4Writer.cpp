#include "Fmp4Writer.h"

#include <cstring>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include "BoxBuilder.h"

namespace fmp4 {

namespace {

// ── Common constants ──────────────────────────────────────────────────────────

// Movie header timescale. Per-track timescales are independent (16000 video,
// 48000 audio); mvhd timescale just defines mp4 movie-level units. 1000 (=1ms)
// is the same value FFmpeg's mp4 muxer uses by default.
constexpr uint32_t kMovieTimescale = 1000;

// 9-entry 3x3 affine matrix in 16.16 fixed point: identity transform.
constexpr uint32_t kIdentityMatrix[9] = {
    0x00010000, 0,          0,
    0,          0x00010000, 0,
    0,          0,          0x40000000
};

// ISO-639-2 packed language: 3 lowercase letters as (c-0x60) in 5-bit slots.
uint16_t PackLanguage(const std::string& lang3) {
    if (lang3.size() < 3) return 0x55C4;  // 'und'
    uint16_t v = 0;
    for (int i = 0; i < 3; ++i) {
        char c = static_cast<char>(lang3[i]);
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        uint16_t off = static_cast<uint16_t>(c - 0x60) & 0x1F;
        v = static_cast<uint16_t>((v << 5) | off);
    }
    return v;
}

// ── Track type helpers ────────────────────────────────────────────────────────

bool IsVideo(const Fmp4Writer::Track& t) {
    return t.codecpar && t.codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
}
bool IsAudio(const Fmp4Writer::Track& t) {
    return t.codecpar && t.codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
}

const char* SampleEntryFourCC(const Fmp4Writer::Track& t) {
    if (!t.codecpar) return "????";
    switch (t.codecpar->codec_id) {
        case AV_CODEC_ID_H264: return "avc1";
        case AV_CODEC_ID_HEVC: return "hev1";  // matches hlsenc-fork's codec_tag choice
        case AV_CODEC_ID_AAC:  return "mp4a";
        default:               return "????";
    }
}

const char* CodecConfigBoxName(const Fmp4Writer::Track& t) {
    if (!t.codecpar) return "";
    switch (t.codecpar->codec_id) {
        case AV_CODEC_ID_H264: return "avcC";
        case AV_CODEC_ID_HEVC: return "hvcC";
        default:               return "";
    }
}

// ── ISO 14496-1 descriptor length encoding (BER-like) ────────────────────────
// We use the 4-byte expanded form (three 0x80 padding bytes + 1 size byte) so
// the descriptor structure has predictable byte offsets, matching what FFmpeg
// emits. Each descriptor is `tag(1) + 0x80 0x80 0x80 size(1) + payload`.
void WriteDescriptorHeader(BoxBuilder& b, uint8_t tag, uint8_t inner_size) {
    b.U8(tag);
    b.U8(0x80);
    b.U8(0x80);
    b.U8(0x80);
    b.U8(inner_size);
}

// ── ftyp ──────────────────────────────────────────────────────────────────────

void WriteFtyp(BoxBuilder& b) {
    auto start = b.BeginBox("ftyp");
    b.Type("iso6");                    // major_brand
    b.U32(1);                          // minor_version
    b.Type("iso6");                    // compat brand
    b.Type("iso5");                    // compat brand
    b.Type("dash");                    // compat brand
    b.EndBox(start);
}

// ── mvhd ──────────────────────────────────────────────────────────────────────

void WriteMvhd(BoxBuilder& b, uint32_t next_track_id) {
    auto start = b.BeginFullBox("mvhd", /*version=*/1, /*flags=*/0);
    b.U64(0);                          // creation_time
    b.U64(0);                          // modification_time
    b.U32(kMovieTimescale);            // timescale
    b.U64(0);                          // duration (0 = fragmented)
    b.U32(0x00010000);                 // rate (1.0)
    b.U16(0x0100);                     // volume (1.0)
    b.U16(0);                          // reserved
    b.U32(0); b.U32(0);                // reserved[2]
    for (uint32_t v : kIdentityMatrix) b.U32(v);
    for (int i = 0; i < 6; ++i) b.U32(0);   // pre_defined[6]
    b.U32(next_track_id);
    b.EndBox(start);
}

// ── trex ──────────────────────────────────────────────────────────────────────
// default_sample_flags = sample_depends_on(1) | is_non_sync(1) = 0x01010000.
// Combined with trun.first_sample_flags = sync, this lets us identify the
// first sample of each fragment as the keyframe without emitting per-sample
// flags in trun (saves 4 bytes per non-key sample).
void WriteTrex(BoxBuilder& b, uint32_t track_id, bool is_video) {
    auto start = b.BeginFullBox("trex", 0, 0);
    b.U32(track_id);
    b.U32(1);                          // default_sample_description_index
    b.U32(0);                          // default_sample_duration
    b.U32(0);                          // default_sample_size
    b.U32(is_video ? 0x01010000u : 0); // default_sample_flags (audio = 0 = sync)
    b.EndBox(start);
}

// ── tkhd ──────────────────────────────────────────────────────────────────────
// Flags: track_enabled | track_in_movie | track_in_preview = 0x07
void WriteTkhd(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginFullBox("tkhd", 1, 0x000007);
    b.U64(0);                          // creation_time
    b.U64(0);                          // modification_time
    b.U32(t.track_id);
    b.U32(0);                          // reserved
    b.U64(0);                          // duration
    b.U32(0); b.U32(0);                // reserved[2]
    b.U16(0);                          // layer
    b.U16(0);                          // alternate_group
    b.U16(IsAudio(t) ? 0x0100 : 0);    // volume
    b.U16(0);                          // reserved
    for (uint32_t v : kIdentityMatrix) b.U32(v);

    if (IsVideo(t) && t.codecpar) {
        b.U32(static_cast<uint32_t>(t.codecpar->width)  << 16);
        b.U32(static_cast<uint32_t>(t.codecpar->height) << 16);
    } else {
        b.U32(0);
        b.U32(0);
    }
    b.EndBox(start);
}

// ── mdhd ──────────────────────────────────────────────────────────────────────

void WriteMdhd(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginFullBox("mdhd", 1, 0);
    b.U64(0);                          // creation_time
    b.U64(0);                          // modification_time
    b.U32(static_cast<uint32_t>(t.timescale.den));   // media timescale
    b.U64(0);                          // duration
    b.U16(t.language ? PackLanguage(*t.language) : PackLanguage("und"));
    b.U16(0);                          // pre_defined
    b.EndBox(start);
}

// ── hdlr ──────────────────────────────────────────────────────────────────────

void WriteHdlr(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginFullBox("hdlr", 0, 0);
    b.U32(0);                          // pre_defined
    if (IsVideo(t)) b.Type("vide");
    else if (IsAudio(t)) b.Type("soun");
    else b.Type("hint");
    b.U32(0); b.U32(0); b.U32(0);      // reserved[3]
    b.String(IsVideo(t) ? "VideoHandler" : "SoundHandler", true);
    b.EndBox(start);
}

// ── vmhd / smhd ──────────────────────────────────────────────────────────────

void WriteVmhd(BoxBuilder& b) {
    auto start = b.BeginFullBox("vmhd", 0, 1);
    b.U16(0);                          // graphicsmode
    b.U16(0); b.U16(0); b.U16(0);      // opcolor[3]
    b.EndBox(start);
}

void WriteSmhd(BoxBuilder& b) {
    auto start = b.BeginFullBox("smhd", 0, 0);
    b.U16(0);                          // balance
    b.U16(0);                          // reserved
    b.EndBox(start);
}

// ── dinf / dref / url ────────────────────────────────────────────────────────

void WriteDinf(BoxBuilder& b) {
    auto dinf = b.BeginBox("dinf");
    {
        auto dref = b.BeginFullBox("dref", 0, 0);
        b.U32(1);                      // entry_count
        {
            // url with self-contained flag (0x000001) means location is empty.
            auto url = b.BeginFullBox("url ", 0, 1);
            b.EndBox(url);
        }
        b.EndBox(dref);
    }
    b.EndBox(dinf);
}

// ── codec config child box (avcC / hvcC) ─────────────────────────────────────

void WriteCodecConfig(BoxBuilder& b, const Fmp4Writer::Track& t) {
    if (!t.codecpar || t.codecpar->extradata_size <= 0) return;
    const char* name = CodecConfigBoxName(t);
    if (!*name) return;

    auto start = b.BeginBox(name);
    b.Bytes(t.codecpar->extradata, t.codecpar->extradata_size);
    b.EndBox(start);
}

// ── esds (for AAC mp4a) ──────────────────────────────────────────────────────
// Layout, all sizes in bytes:
//   ESDescriptor [03 80 80 80 sz_es]
//     ES_ID (2)  flags (1)
//     DecoderConfigDescriptor [04 80 80 80 sz_dcd]
//       objectTypeIndication=0x40 (1)  streamType<<2|reserved|upStream (1)
//       bufferSizeDB (3)
//       maxBitrate (4)
//       avgBitrate (4)
//       DecoderSpecificInfo [05 80 80 80 sz_dsi]
//         AudioSpecificConfig (extradata bytes)
//     SLConfigDescriptor [06 80 80 80 01]
//       predefined = 0x02
void WriteEsds(BoxBuilder& b, const Fmp4Writer::Track& t) {
    if (!t.codecpar) return;

    uint8_t dsi_size = static_cast<uint8_t>(t.codecpar->extradata_size);
    uint8_t dcd_inner_size = static_cast<uint8_t>(13 + 5 + dsi_size);
    uint8_t es_inner_size = static_cast<uint8_t>(3 + 5 + dcd_inner_size + 5 + 1);

    auto start = b.BeginFullBox("esds", 0, 0);

    WriteDescriptorHeader(b, 0x03, es_inner_size);   // ES_Descriptor
    b.U16(0);                                        // ES_ID
    b.U8(0);                                         // flags

    WriteDescriptorHeader(b, 0x04, dcd_inner_size);  // DecoderConfigDescriptor
    b.U8(0x40);                                      // AAC LC
    b.U8(0x15);                                      // streamType=audio (5<<2)|0|1
    b.U24(0);                                        // bufferSizeDB
    b.U32(0);                                        // maxBitrate
    b.U32(static_cast<uint32_t>(t.codecpar->bit_rate ? t.codecpar->bit_rate : 128000));

    WriteDescriptorHeader(b, 0x05, dsi_size);        // DecoderSpecificInfo
    if (dsi_size > 0) b.Bytes(t.codecpar->extradata, dsi_size);

    WriteDescriptorHeader(b, 0x06, 1);               // SLConfigDescriptor
    b.U8(0x02);

    b.EndBox(start);
}

// ── sample entry inside stsd ─────────────────────────────────────────────────

void WriteVideoSampleEntry(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginBox(SampleEntryFourCC(t));
    // SampleEntry header (8 bytes)
    for (int i = 0; i < 6; ++i) b.U8(0);             // reserved[6]
    b.U16(1);                                        // data_reference_index

    // VisualSampleEntry (70 bytes)
    b.U16(0);                                        // pre_defined
    b.U16(0);                                        // reserved
    b.U32(0); b.U32(0); b.U32(0);                    // pre_defined[3]
    b.U16(static_cast<uint16_t>(t.codecpar->width));
    b.U16(static_cast<uint16_t>(t.codecpar->height));
    b.U32(0x00480000);                               // horizresolution = 72 dpi
    b.U32(0x00480000);                               // vertresolution = 72 dpi
    b.U32(0);                                        // reserved
    b.U16(1);                                        // frame_count
    b.U8(0);                                         // compressorname (length byte = 0)
    for (int i = 0; i < 31; ++i) b.U8(0);            // compressorname padding (total 32)
    b.U16(0x0018);                                   // depth
    b.U16(0xFFFF);                                   // pre_defined = -1

    WriteCodecConfig(b, t);                          // avcC / hvcC

    b.EndBox(start);
}

void WriteAudioSampleEntry(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginBox(SampleEntryFourCC(t));
    for (int i = 0; i < 6; ++i) b.U8(0);             // reserved[6]
    b.U16(1);                                        // data_reference_index

    // AudioSampleEntry
    b.U32(0); b.U32(0);                              // reserved[2]
    int channels = t.codecpar->ch_layout.nb_channels > 0
        ? t.codecpar->ch_layout.nb_channels : 2;
    b.U16(static_cast<uint16_t>(channels));          // channelcount
    b.U16(16);                                       // samplesize
    b.U16(0);                                        // pre_defined
    b.U16(0);                                        // reserved
    uint32_t sr = t.codecpar->sample_rate > 0 ? t.codecpar->sample_rate : 48000;
    b.U32(sr << 16);                                 // samplerate (16.16)

    WriteEsds(b, t);

    b.EndBox(start);
}

void WriteStsd(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginFullBox("stsd", 0, 0);
    b.U32(1);                                        // entry_count
    if (IsVideo(t)) WriteVideoSampleEntry(b, t);
    else if (IsAudio(t)) WriteAudioSampleEntry(b, t);
    b.EndBox(start);
}

// ── stbl ──────────────────────────────────────────────────────────────────────

void WriteStbl(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginBox("stbl");

    WriteStsd(b, t);

    {  // empty stts
        auto p = b.BeginFullBox("stts", 0, 0);
        b.U32(0);
        b.EndBox(p);
    }
    {  // empty stsc
        auto p = b.BeginFullBox("stsc", 0, 0);
        b.U32(0);
        b.EndBox(p);
    }
    {  // empty stsz
        auto p = b.BeginFullBox("stsz", 0, 0);
        b.U32(0);                                    // sample_size
        b.U32(0);                                    // sample_count
        b.EndBox(p);
    }
    {  // empty stco
        auto p = b.BeginFullBox("stco", 0, 0);
        b.U32(0);
        b.EndBox(p);
    }

    b.EndBox(start);
}

// ── minf ──────────────────────────────────────────────────────────────────────

void WriteMinf(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginBox("minf");
    if (IsVideo(t)) WriteVmhd(b);
    else if (IsAudio(t)) WriteSmhd(b);
    WriteDinf(b);
    WriteStbl(b, t);
    b.EndBox(start);
}

// ── mdia ──────────────────────────────────────────────────────────────────────

void WriteMdia(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginBox("mdia");
    WriteMdhd(b, t);
    WriteHdlr(b, t);
    WriteMinf(b, t);
    b.EndBox(start);
}

// ── edts/elst (audio AAC priming) ────────────────────────────────────────────
// elst is emitted only when initial_padding > 0. The single edit entry skips
// `initial_padding` samples at playback start, matching what FFmpeg emits.
void WriteEdtsForPriming(BoxBuilder& b, const Fmp4Writer::Track& t) {
    if (t.initial_padding <= 0) return;

    auto edts = b.BeginBox("edts");
    {
        auto elst = b.BeginFullBox("elst", 1, 0);
        b.U32(1);                                    // entry_count
        b.U64(0);                                    // segment_duration (0 = until end)
        b.S64(t.initial_padding);                    // media_time (positive = skip)
        b.U16(1);                                    // media_rate_integer
        b.U16(0);                                    // media_rate_fraction
        b.EndBox(elst);
    }
    b.EndBox(edts);
}

// ── trak ──────────────────────────────────────────────────────────────────────

void WriteTrak(BoxBuilder& b, const Fmp4Writer::Track& t) {
    auto start = b.BeginBox("trak");
    WriteTkhd(b, t);
    if (IsAudio(t)) WriteEdtsForPriming(b, t);
    WriteMdia(b, t);
    b.EndBox(start);
}

// ── moov ──────────────────────────────────────────────────────────────────────

void WriteMoov(BoxBuilder& b, const std::vector<Fmp4Writer::Track>& tracks) {
    auto start = b.BeginBox("moov");

    uint32_t max_tid = 0;
    for (const auto& t : tracks) max_tid = std::max(max_tid, t.track_id);
    WriteMvhd(b, max_tid + 1);

    {
        auto mvex = b.BeginBox("mvex");
        for (const auto& t : tracks) WriteTrex(b, t.track_id, IsVideo(t));
        b.EndBox(mvex);
    }

    for (const auto& t : tracks) WriteTrak(b, t);

    b.EndBox(start);
}

// ── styp ──────────────────────────────────────────────────────────────────────
// CMAF-style segment type. Bytes match hlsenc-fork's hardcoded `kHlsStypBox`
// to maintain output parity (hls.js / Safari accept these brands fine).
void WriteStyp(BoxBuilder& b) {
    auto start = b.BeginBox("styp");
    b.Type("msdh");                    // major_brand
    b.U32(0);                          // minor_version
    b.Type("msdh");                    // compat brand
    b.Type("msix");                    // compat brand
    b.EndBox(start);
}

// ── sidx (per track) ─────────────────────────────────────────────────────────
// Single-reference sidx pointing at the moof+mdat that follows. EPT/duration
// are in the track's own timescale.
//
// Returns the buffer offset of the `referenced_size` field so callers can
// patch it after the moof+mdat is written.
size_t WriteSidx(BoxBuilder& b, uint32_t reference_id, uint32_t timescale,
                 int64_t earliest_presentation_time, uint32_t subsegment_duration) {
    auto start = b.BeginFullBox("sidx", 1, 0);
    b.U32(reference_id);
    b.U32(timescale);
    b.S64(earliest_presentation_time);
    b.U64(0);                          // first_offset (0; references start
                                       //   immediately after this sidx)
    b.U16(0);                          // reserved
    b.U16(1);                          // reference_count
    size_t ref_size_offset = b.Pos();
    b.U32(0);                          // referenced_size (patched by caller)
    b.U32(subsegment_duration);
    b.U32(0x90000000);                 // starts_with_SAP=1, SAP_type=1, SAP_delta=0
    b.EndBox(start);
    return ref_size_offset;
}

// ── mfhd ──────────────────────────────────────────────────────────────────────

void WriteMfhd(BoxBuilder& b, uint32_t sequence_number) {
    auto start = b.BeginFullBox("mfhd", 0, 0);
    b.U32(sequence_number);
    b.EndBox(start);
}

// ── tfhd ──────────────────────────────────────────────────────────────────────
// Flag 0x020000 = default-base-is-moof (data_offset in trun is relative to
// the start of the enclosing moof). Required by CMAF.
void WriteTfhd(BoxBuilder& b, uint32_t track_id) {
    auto start = b.BeginFullBox("tfhd", 0, 0x020000);
    b.U32(track_id);
    b.EndBox(start);
}

// ── tfdt ──────────────────────────────────────────────────────────────────────

void WriteTfdt(BoxBuilder& b, int64_t base_media_decode_time) {
    auto start = b.BeginFullBox("tfdt", 1, 0);
    b.S64(base_media_decode_time);
    b.EndBox(start);
}

// ── trun ──────────────────────────────────────────────────────────────────────
// trun flags we always set:
//   0x000001  data_offset_present
//   0x000004  first_sample_flags_present
//   0x000100  sample_duration_present
//   0x000200  sample_size_present
//   0x000800  sample_composition_time_offsets_present (signed, version 1)
constexpr uint32_t kTrunFlags = 0x000001u | 0x000004u | 0x000100u | 0x000200u | 0x000800u;

// Sample flag word for a key sample (sample_depends_on=2, is_non_sync=0).
// Used as trun.first_sample_flags to mark the segment-leading keyframe.
constexpr uint32_t kSampleFlagsKey    = 0x02000000u;

// Returns the buffer offset of the `data_offset` field so callers can patch
// it once the moof's total size is known.
size_t WriteTrun(BoxBuilder& b, const std::vector<Sample>& samples) {
    auto start = b.BeginFullBox("trun", 1, kTrunFlags);
    b.U32(static_cast<uint32_t>(samples.size()));   // sample_count
    size_t data_offset_pos = b.Pos();
    b.S32(0);                                       // data_offset (patched later)
    // first_sample_flags: first sample is the segment's keyframe.
    b.U32(kSampleFlagsKey);

    for (const auto& s : samples) {
        b.U32(s.duration);
        b.U32(s.size);
        b.S32(s.cts_offset);
    }
    b.EndBox(start);
    return data_offset_pos;
}

}  // namespace

// ── BuildInit ────────────────────────────────────────────────────────────────

std::vector<uint8_t> Fmp4Writer::BuildInit(const std::vector<Track>& tracks) {
    if (tracks.empty()) throw std::runtime_error("Fmp4Writer::BuildInit: no tracks");

    BoxBuilder b;
    WriteFtyp(b);
    WriteMoov(b, tracks);
    return b.Take();
}

// ── BuildSegment ─────────────────────────────────────────────────────────────

std::vector<uint8_t> Fmp4Writer::BuildSegment(
        uint32_t sequence_number,
        const std::vector<Track>& tracks,
        const std::vector<std::vector<Sample>>& samples_per_track,
        const std::vector<int64_t>& tfdt_per_track) {

    if (tracks.size() != samples_per_track.size() ||
        tracks.size() != tfdt_per_track.size()) {
        throw std::runtime_error("Fmp4Writer::BuildSegment: track/sample/tfdt size mismatch");
    }

    BoxBuilder b;

    WriteStyp(b);

    // ── sidx per track ───────────────────────────────────────────────────────
    // We need the offset of each sidx's referenced_size field so we can patch
    // it after we know the moof+mdat size.
    std::vector<size_t> sidx_ref_size_offsets;
    sidx_ref_size_offsets.reserve(tracks.size());
    size_t first_sidx_start = b.Pos();
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        const auto& samples = samples_per_track[i];

        // EPT = first sample's PTS (DTS + cts_offset)
        int64_t ept = tfdt_per_track[i];
        uint32_t subsegment_duration = 0;
        if (!samples.empty()) {
            ept = samples.front().dts + samples.front().cts_offset;
            int64_t total = 0;
            for (const auto& s : samples) total += s.duration;
            subsegment_duration = static_cast<uint32_t>(total);
        }

        size_t off = WriteSidx(b,
            /*reference_id=*/t.track_id,
            /*timescale=*/static_cast<uint32_t>(t.timescale.den),
            /*earliest_presentation_time=*/ept,
            /*subsegment_duration=*/subsegment_duration);
        sidx_ref_size_offsets.push_back(off);
    }
    size_t after_sidx = b.Pos();
    uint32_t sidx_total_size = static_cast<uint32_t>(after_sidx - first_sidx_start);

    // ── moof ────────────────────────────────────────────────────────────────
    size_t moof_start = b.BeginBox("moof");
    WriteMfhd(b, sequence_number);

    std::vector<size_t> data_offset_positions;
    data_offset_positions.reserve(tracks.size());

    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        const auto& samples = samples_per_track[i];

        auto traf_start = b.BeginBox("traf");
        WriteTfhd(b, t.track_id);
        WriteTfdt(b, tfdt_per_track[i]);

        size_t trun_data_off_pos = 0;
        if (!samples.empty()) {
            trun_data_off_pos = WriteTrun(b, samples);
        }
        data_offset_positions.push_back(trun_data_off_pos);

        b.EndBox(traf_start);
    }

    b.EndBox(moof_start);

    // ── mdat ────────────────────────────────────────────────────────────────
    size_t mdat_start = b.BeginBox("mdat");

    // Per-track payload start offsets (relative to the start of moof).
    std::vector<size_t> track_payload_offsets_in_moof;
    track_payload_offsets_in_moof.reserve(tracks.size());

    for (size_t i = 0; i < tracks.size(); ++i) {
        size_t track_payload_start = b.Pos();
        track_payload_offsets_in_moof.push_back(track_payload_start - moof_start);
        const auto& samples = samples_per_track[i];
        for (const auto& s : samples) {
            if (s.size > 0 && s.data != nullptr) {
                b.Bytes(s.data, s.size);
            }
        }
    }
    b.EndBox(mdat_start);
    size_t total_segment_size = b.Pos();

    // ── Patch trun.data_offset (relative to moof start) ─────────────────────
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (data_offset_positions[i] == 0) continue;
        b.Patch32(data_offset_positions[i],
                  static_cast<uint32_t>(track_payload_offsets_in_moof[i]));
    }

    // ── Patch sidx referenced_size: bytes from the END of all sidx boxes
    // through the end of mdat (i.e. moof+mdat together, since multi-track
    // sidx all reference the same moof+mdat blob).
    uint32_t referenced_size = static_cast<uint32_t>(total_segment_size - after_sidx);
    for (size_t off : sidx_ref_size_offsets) {
        // High bit is reference_type (0 = media), low 31 bits are referenced_size.
        b.Patch32(off, referenced_size & 0x7FFFFFFFu);
    }
    (void)sidx_total_size;  // diagnostic only; unused at runtime

    return b.Take();
}

}  // namespace fmp4
