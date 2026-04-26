#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fmp4 {

// Low-level big-endian byte writer plus ISO BMFF box helpers.
//
// Two patterns are supported for emitting boxes:
//
//   1. Leaf box / FullBox with known length: build the inner bytes, then
//      call WriteBox / WriteFullBox to prepend the size+type+(version/flags).
//
//   2. Container box with backpatched size: call BeginBox(...) to record
//      the start offset and reserve a 32-bit size placeholder, write
//      children, then EndBox(start) which patches the size in place.
class BoxBuilder {
public:
    BoxBuilder() = default;

    void U8 (uint8_t  v) { buf_.push_back(v); }
    void U16(uint16_t v) { buf_.push_back(v >> 8); buf_.push_back(v & 0xFF); }
    void U24(uint32_t v) {
        buf_.push_back((v >> 16) & 0xFF);
        buf_.push_back((v >> 8)  & 0xFF);
        buf_.push_back( v        & 0xFF);
    }
    void U32(uint32_t v) {
        buf_.push_back((v >> 24) & 0xFF);
        buf_.push_back((v >> 16) & 0xFF);
        buf_.push_back((v >> 8)  & 0xFF);
        buf_.push_back( v        & 0xFF);
    }
    void U64(uint64_t v) {
        U32(static_cast<uint32_t>(v >> 32));
        U32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
    }
    void S32(int32_t v) { U32(static_cast<uint32_t>(v)); }
    void S64(int64_t v) { U64(static_cast<uint64_t>(v)); }

    void Bytes(const uint8_t* data, size_t len) {
        if (len == 0 || !data) return;
        buf_.insert(buf_.end(), data, data + len);
    }
    void Bytes(const std::vector<uint8_t>& v) { Bytes(v.data(), v.size()); }
    void Type(const char fourcc[4]) { Bytes(reinterpret_cast<const uint8_t*>(fourcc), 4); }
    void String(const std::string& s, bool null_terminate = true) {
        Bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        if (null_terminate) U8(0);
    }
    void Zeros(size_t n) { buf_.insert(buf_.end(), n, uint8_t{0}); }

    // ── Box helpers ─────────────────────────────────────────────────────────

    // Begin a box and return the offset where its size header sits. Reserves
    // 4 bytes for the size (patched by EndBox) and emits the 4-byte type.
    size_t BeginBox(const char type[4]) {
        size_t start = buf_.size();
        U32(0);             // size placeholder
        Type(type);
        return start;
    }

    // Begin a FullBox: same as BeginBox + 1 byte version + 3 bytes flags.
    size_t BeginFullBox(const char type[4], uint8_t version, uint32_t flags) {
        size_t start = BeginBox(type);
        U8(version);
        U24(flags);
        return start;
    }

    // Patch the 32-bit size of the box that started at `start_offset`.
    void EndBox(size_t start_offset) {
        uint32_t size = static_cast<uint32_t>(buf_.size() - start_offset);
        buf_[start_offset]     = (size >> 24) & 0xFF;
        buf_[start_offset + 1] = (size >> 16) & 0xFF;
        buf_[start_offset + 2] = (size >> 8)  & 0xFF;
        buf_[start_offset + 3] =  size        & 0xFF;
    }

    // Patch a 32-bit value at an arbitrary offset (used for trun.data_offset).
    void Patch32(size_t offset, uint32_t value) {
        buf_[offset]     = (value >> 24) & 0xFF;
        buf_[offset + 1] = (value >> 16) & 0xFF;
        buf_[offset + 2] = (value >> 8)  & 0xFF;
        buf_[offset + 3] =  value        & 0xFF;
    }

    size_t Pos() const { return buf_.size(); }
    std::vector<uint8_t>& buffer() { return buf_; }
    const std::vector<uint8_t>& buffer() const { return buf_; }
    std::vector<uint8_t> Take() { return std::move(buf_); }

    // ── ISO BMFF four-character constants ───────────────────────────────────
    // Convenience for building 4-byte type strings without trailing NULs.
    static constexpr const char* k_ftyp = "ftyp";
    static constexpr const char* k_moov = "moov";
    static constexpr const char* k_styp = "styp";
    static constexpr const char* k_sidx = "sidx";
    static constexpr const char* k_moof = "moof";
    static constexpr const char* k_mdat = "mdat";

private:
    std::vector<uint8_t> buf_;
};

}  // namespace fmp4
