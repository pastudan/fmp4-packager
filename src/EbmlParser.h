#pragma once

// Minimal EBML/Matroska parser used to locate the Cues element in an MKV file.
//
// MKV structure relevant to us:
//   EBML header  (0x1A45DFA3)
//   Segment      (0x18538067)
//     SeekHead   (0x114D9B74)   ← always near top of Segment
//       Seek     (0x4DBB)
//         SeekID       (0x53AB) = element ID this entry refers to
//         SeekPosition (0x53AC) = byte offset from Segment data start
//     ... more level-1 elements ...
//     Cues       (0x1C53BB6B)   ← anywhere in the file
//
// We only need the first ~256 KB to find the SeekHead.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>

struct CuesLocation {
    int64_t offset;  // byte offset from start of file
    int64_t size;    // byte size of the Cues element (0 if unknown)
    int64_t segment_data_start; // byte offset of first byte after the Segment header
};

// ── EBML variable-length integer decoding ────────────────────────────────────

// Read an EBML ID (vint with marker bit kept).
// Returns the number of bytes consumed, or 0 on error.
static inline int ebml_read_id(const uint8_t* buf, int len, uint64_t& out_id)
{
    if (!buf || len < 1) return 0;
    uint8_t first = buf[0];
    if (first == 0) return 0;  // 0x00 is never a valid EBML ID lead byte
    int width = 0;
    if      (first & 0x80) width = 1;
    else if (first & 0x40) width = 2;
    else if (first & 0x20) width = 3;
    else if (first & 0x10) width = 4;
    else if (first & 0x08) width = 5;
    else if (first & 0x04) width = 6;
    else if (first & 0x02) width = 7;
    else if (first & 0x01) width = 8;
    if (width == 0) return 0;

    if (len < width) return 0;

    uint64_t id = 0;
    for (int i = 0; i < width; ++i)
        id = (id << 8) | buf[i];
    out_id = id;
    return width;
}

// Read an EBML size vint (marker bit cleared, 0xFF…F = unknown size).
// Returns bytes consumed, or 0 on error.
static inline int ebml_read_size(const uint8_t* buf, int len, int64_t& out_size)
{
    if (!buf || len < 1) return 0;
    uint8_t first = buf[0];
    if (first == 0) return 0;  // 0x00 is never a valid EBML size lead byte
    int width = 0;
    uint8_t mask = 0;
    if      (first & 0x80) { width = 1; mask = 0x7f; }
    else if (first & 0x40) { width = 2; mask = 0x3f; }
    else if (first & 0x20) { width = 3; mask = 0x1f; }
    else if (first & 0x10) { width = 4; mask = 0x0f; }
    else if (first & 0x08) { width = 5; mask = 0x07; }
    else if (first & 0x04) { width = 6; mask = 0x03; }
    else if (first & 0x02) { width = 7; mask = 0x01; }
    else if (first & 0x01) { width = 8; mask = 0x00; }
    if (width == 0) return 0;

    if (len < width) return 0;

    int64_t val = (int64_t)(buf[0] & mask);
    for (int i = 1; i < width; ++i)
        val = (val << 8) | buf[i];

    // All-ones = unknown/infinite size
    out_size = val;
    return width;
}

// Read a uint stored as raw big-endian bytes (for SeekPosition / SeekID values).
static inline uint64_t ebml_read_uint(const uint8_t* buf, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len && i < 8; ++i)
        v = (v << 8) | buf[i];
    return v;
}

// ── SeekHead scanner ──────────────────────────────────────────────────────────

// EBML/Matroska element IDs we care about.
static constexpr uint64_t kEbmlId_EBMLHeader  = 0x1A45DFA3ULL;
static constexpr uint64_t kEbmlId_Segment     = 0x18538067ULL;
static constexpr uint64_t kEbmlId_SeekHead    = 0x114D9B74ULL;
static constexpr uint64_t kEbmlId_Seek        = 0x4DBBULL;
static constexpr uint64_t kEbmlId_SeekID      = 0x53ABULL;
static constexpr uint64_t kEbmlId_SeekPos     = 0x53ACULL;
static constexpr uint64_t kEbmlId_Cues        = 0x1C53BB6BULL;

// Scan a flat list of EBML children in [buf, buf+len).
// Calls visitor(id, data_ptr, data_size) for each child element found.
// child_base_offset is the file offset of buf[0] (used so visitor gets absolute offsets).
template<typename Visitor>
static void ebml_scan_children(const uint8_t* buf, int len, int64_t child_base_offset,
                                Visitor&& visitor)
{
    if (!buf || len <= 0) return;

    int pos = 0;
    while (pos >= 0 && pos < len) {
        uint64_t id = 0;
        int64_t  sz = 0;

        int id_bytes = ebml_read_id(buf + pos, len - pos, id);
        if (id_bytes == 0) break;
        pos += id_bytes;
        if (pos >= len) break;

        int sz_bytes = ebml_read_size(buf + pos, len - pos, sz);
        if (sz_bytes == 0) break;
        pos += sz_bytes;
        if (pos > len) break;

        int data_remaining = len - pos;
        if (data_remaining <= 0) break;

        int data_avail = (sz > 0 && sz < (int64_t)data_remaining)
                         ? (int)sz : data_remaining;

        visitor(id, buf + pos, data_avail, sz, child_base_offset + pos);

        if (sz <= 0) break;  // unknown/zero size — can't skip
        if (sz > (int64_t)(len - pos)) break;  // element extends beyond buffer
        pos += (int)sz;
    }
}

// Parse the Cues location from a buffer containing the beginning of an MKV file.
// buf should be at least ~256 KB (the SeekHead is always within the first few KB).
// Returns {absolute_file_offset, size_in_bytes} of the Cues element, or nullopt.
// ── Segment Info parser ──────────────────────────────────────────────────────

static constexpr uint64_t kEbmlId_Info           = 0x1549A966ULL;
static constexpr uint64_t kEbmlId_TimestampScale = 0x2AD7B1ULL;
static constexpr uint64_t kEbmlId_Tracks         = 0x1654AE6BULL;
static constexpr uint64_t kEbmlId_TrackEntry     = 0xAEULL;
static constexpr uint64_t kEbmlId_TrackNumber    = 0xD7ULL;
static constexpr uint64_t kEbmlId_TrackType      = 0x83ULL;

struct SegmentInfo {
    int64_t timestamp_scale = 1000000;  // nanoseconds per tick, default 1ms
    int video_track_number = 1;          // 1-based track number
};

// Parse Segment Info and Tracks from header to get TimestampScale and video track number
inline SegmentInfo parse_segment_info(const uint8_t* buf, int len)
{
    SegmentInfo info;
    if (!buf || len < 32) return info;

    int pos = 0;

    // Skip EBML header
    ebml_scan_children(buf, len, 0, [&](uint64_t id, const uint8_t*, int, int64_t sz, int64_t offset) {
        if (id == kEbmlId_EBMLHeader) {
            pos = (int)(offset + sz);
        }
    });

    if (pos == 0 || pos >= len) return info;

    // Parse Segment header
    uint64_t seg_id = 0;
    int64_t seg_sz = 0;
    int id_b = ebml_read_id(buf + pos, len - pos, seg_id);
    if (id_b == 0 || seg_id != kEbmlId_Segment) return info;
    pos += id_b;

    int sz_b = ebml_read_size(buf + pos, len - pos, seg_sz);
    if (sz_b == 0) return info;
    pos += sz_b;

    // Scan Segment children for Info and Tracks
    ebml_scan_children(buf + pos, len - pos, pos,
        [&](uint64_t id, const uint8_t* data, int avail, int64_t, int64_t) {
            if (id == kEbmlId_Info) {
                // Parse Info children for TimestampScale
                ebml_scan_children(data, avail, 0,
                    [&](uint64_t iid, const uint8_t* idata, int iavail, int64_t, int64_t) {
                        if (iid == kEbmlId_TimestampScale) {
                            info.timestamp_scale = (int64_t)ebml_read_uint(idata, iavail);
                        }
                    });
            } else if (id == kEbmlId_Tracks) {
                // Parse Tracks children for video track number
                ebml_scan_children(data, avail, 0,
                    [&](uint64_t tid, const uint8_t* tdata, int tavail, int64_t, int64_t) {
                        if (tid != kEbmlId_TrackEntry) return;

                        int track_num = 0;
                        int track_type = 0;
                        ebml_scan_children(tdata, tavail, 0,
                            [&](uint64_t eid, const uint8_t* edata, int eavail, int64_t, int64_t) {
                                if (eid == kEbmlId_TrackNumber) {
                                    track_num = (int)ebml_read_uint(edata, eavail);
                                } else if (eid == kEbmlId_TrackType) {
                                    track_type = (int)ebml_read_uint(edata, eavail);
                                }
                            });

                        // TrackType 1 = video
                        if (track_type == 1 && track_num > 0) {
                            info.video_track_number = track_num;
                        }
                    });
            }
        });

    return info;
}

// ── Cues parser ──────────────────────────────────────────────────────────────

// Additional element IDs for parsing Cues
static constexpr uint64_t kEbmlId_CuePoint     = 0xBBULL;
static constexpr uint64_t kEbmlId_CueTime      = 0xB3ULL;
static constexpr uint64_t kEbmlId_CueTrackPos  = 0xB7ULL;
static constexpr uint64_t kEbmlId_CueTrack     = 0xF7ULL;
static constexpr uint64_t kEbmlId_CueClusterPos = 0xF1ULL;
static constexpr uint64_t kEbmlId_CueRelativePos = 0xF0ULL;

struct CueEntry {
    int64_t time_ms;     // CueTime in milliseconds (after applying TimestampScale)
    int64_t timestamp;   // Raw timestamp for FFmpeg index
    int track_number;    // Matroska TrackNumber
    int64_t cluster_pos; // Absolute file offset of the referenced Cluster
    int64_t relative_pos;// Relative block position inside Cluster, or -1 if absent
};

// Parse Cues element data to extract keyframe timestamps.
// timestamp_scale is from the Segment Info (default 1000000 = 1ms per unit).
// video_track_number is the track we want keyframes for (1-based).
// Returns vector of {time_ms, raw_timestamp} pairs.
inline std::vector<CueEntry> parse_cues_data(const uint8_t* buf, int len,
                                              int64_t timestamp_scale = 1000000,
                                              int video_track_number = 1,
                                              int64_t segment_data_start = 0)
{
    std::vector<CueEntry> entries;
    if (!buf || len < 4) return entries;

    // Skip the Cues element header (ID + size) to get to children
    int pos = 0;
    uint64_t cues_id = 0;
    int64_t cues_sz = 0;

    int id_b = ebml_read_id(buf, len, cues_id);
    if (id_b == 0 || cues_id != kEbmlId_Cues) return entries;
    pos += id_b;

    int sz_b = ebml_read_size(buf + pos, len - pos, cues_sz);
    if (sz_b == 0) return entries;
    pos += sz_b;

    // Now parse CuePoint children
    ebml_scan_children(buf + pos, len - pos, pos,
        [&](uint64_t id, const uint8_t* data, int avail, int64_t sz, int64_t /*offset*/) {
            if (id != kEbmlId_CuePoint) return;

            int64_t cue_time = -1;
            bool has_video_track = false;
            int video_track = 0;
            int64_t video_cluster_pos = -1;
            int64_t video_relative_pos = -1;

            // Parse CuePoint children
            ebml_scan_children(data, avail, 0,
                [&](uint64_t cid, const uint8_t* cdata, int cavail, int64_t /*csz*/, int64_t /*co*/) {
                    if (cid == kEbmlId_CueTime) {
                        cue_time = (int64_t)ebml_read_uint(cdata, cavail);
                    } else if (cid == kEbmlId_CueTrackPos) {
                        // Parse CueTrackPositions to check track number
                        int track = 0;
                        int64_t cluster_pos = -1;
                        int64_t relative_pos = -1;
                        ebml_scan_children(cdata, cavail, 0,
                            [&](uint64_t tid, const uint8_t* tdata, int tavail, int64_t, int64_t) {
                                if (tid == kEbmlId_CueTrack) {
                                    track = (int)ebml_read_uint(tdata, tavail);
                                } else if (tid == kEbmlId_CueClusterPos) {
                                    cluster_pos = (int64_t)ebml_read_uint(tdata, tavail);
                                } else if (tid == kEbmlId_CueRelativePos) {
                                    relative_pos = (int64_t)ebml_read_uint(tdata, tavail);
                                }
                            });

                        if (track == video_track_number) {
                            has_video_track = true;
                            video_track = track;
                            video_cluster_pos = cluster_pos;
                            video_relative_pos = relative_pos;
                        }
                    }
                });

            if (cue_time >= 0 && has_video_track) {
                // Convert to milliseconds using TimestampScale
                // TimestampScale is nanoseconds per tick, default 1000000 (= 1ms)
                int64_t time_ms = (cue_time * timestamp_scale) / 1000000;
                int64_t absolute_cluster_pos = -1;
                if (video_cluster_pos >= 0 && segment_data_start >= 0) {
                    absolute_cluster_pos = segment_data_start + video_cluster_pos;
                }
                entries.push_back(CueEntry{time_ms, cue_time, video_track,
                                           absolute_cluster_pos, video_relative_pos});
            }
        });

    return entries;
}

// ── SeekHead scanner ─────────────────────────────────────────────────────────

inline std::optional<CuesLocation> parse_cues_location(const uint8_t* buf, int len)
{
    if (!buf || len < 32) return std::nullopt;
    int pos = 0;
    int64_t segment_data_start = -1;

    // Skip the EBML header element
    ebml_scan_children(buf, len, 0, [&](uint64_t id, const uint8_t* data, int avail,
                                         int64_t sz, int64_t data_offset) {
        if (id == kEbmlId_EBMLHeader) {
            // data_offset is start of EBML header data; pos after this element =
            // data_offset + sz
            pos = (int)(data_offset + sz);
        }
    });

    if (pos == 0) return std::nullopt;  // couldn't find EBML header

    // Now pos points to the Segment element
    if (pos >= len) return std::nullopt;

    uint64_t seg_id = 0;
    int64_t  seg_sz = 0;
    int id_b = ebml_read_id(buf + pos, len - pos, seg_id);
    if (id_b == 0 || seg_id != kEbmlId_Segment) return std::nullopt;
    pos += id_b;

    int sz_b = ebml_read_size(buf + pos, len - pos, seg_sz);
    if (sz_b == 0) return std::nullopt;
    pos += sz_b;

    segment_data_start = pos;  // absolute file offset of first byte after Segment header

    // Scan Segment children looking for SeekHead
    std::optional<CuesLocation> result;

    ebml_scan_children(buf + pos, len - pos, (int64_t)pos,
        [&](uint64_t id, const uint8_t* data, int avail, int64_t sz, int64_t data_offset) {
            if (id != kEbmlId_SeekHead) return;

            // Parse SeekHead children (Seek entries)
            ebml_scan_children(data, avail, data_offset,
                [&](uint64_t sid, const uint8_t* sdata, int savail, int64_t ssz,
                    int64_t sdata_offset) {
                    if (sid != kEbmlId_Seek) return;

                    // Parse Seek children to get SeekID + SeekPosition
                    uint64_t seek_id_val  = 0;
                    uint64_t seek_pos_val = UINT64_MAX;
                    bool     got_id = false, got_pos = false;

                    ebml_scan_children(sdata, savail, sdata_offset,
                        [&](uint64_t cid, const uint8_t* cdata, int cavail, int64_t /*csz*/,
                            int64_t /*cdata_offset*/) {
                            if (cid == kEbmlId_SeekID) {
                                seek_id_val = ebml_read_uint(cdata, cavail);
                                got_id = true;
                            } else if (cid == kEbmlId_SeekPos) {
                                seek_pos_val = ebml_read_uint(cdata, cavail);
                                got_pos = true;
                            }
                        });

                    if (got_id && got_pos && seek_id_val == kEbmlId_Cues) {
                        // SeekPosition is relative to Segment data start
                        int64_t abs_offset = segment_data_start + (int64_t)seek_pos_val;

                        // Try to read the Cues element size from buf if in range
                        int64_t cues_sz = 0;
                        if (abs_offset < len) {
                            int cues_pos = (int)abs_offset;
                            uint64_t cues_id = 0;
                            int cid_b = ebml_read_id(buf + cues_pos, len - cues_pos, cues_id);
                            if (cid_b > 0 && cues_id == kEbmlId_Cues) {
                                cues_pos += cid_b;
                                int64_t csz = 0;
                                int csz_b = ebml_read_size(buf + cues_pos, len - cues_pos, csz);
                                if (csz_b > 0 && csz > 0) {
                                    cues_sz = (int64_t)cid_b + csz_b + csz;
                                }
                            }
                        }

                        result = CuesLocation{ abs_offset, cues_sz, segment_data_start };
                    }
                });
        });

    return result;
}
