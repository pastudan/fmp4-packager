// Smoke tests for BoxBuilder.
//
// Covers:
//   - big-endian integer writes (u8/u16/u24/u32/u64, signed wrappers)
//   - FullBox / leaf box size backpatch
//   - nested container boxes
//   - Patch32 mid-buffer
//
// Build with:  cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build
// Run    with: ./build/test_box_builder

#include "BoxBuilder.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using fmp4::BoxBuilder;

namespace {

#define EXPECT(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        std::exit(1); \
    } \
} while (0)

void TestEndianWrites() {
    BoxBuilder b;
    b.U8(0xAB);
    b.U16(0xCAFE);
    b.U24(0x123456);
    b.U32(0xDEADBEEF);
    b.U64(0x0123456789ABCDEFULL);

    const std::vector<uint8_t> expected{
        0xAB,
        0xCA, 0xFE,
        0x12, 0x34, 0x56,
        0xDE, 0xAD, 0xBE, 0xEF,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    };
    EXPECT(b.buffer() == expected);
}

void TestSignedWrappers() {
    BoxBuilder b;
    b.S32(-1);
    b.S64(-2);
    EXPECT(b.buffer().size() == 12);
    EXPECT(b.buffer()[0] == 0xFF && b.buffer()[3] == 0xFF);  // -1 as u32
    EXPECT(b.buffer()[4] == 0xFF && b.buffer()[11] == 0xFE); // -2 as u64
}

void TestLeafBox() {
    BoxBuilder b;
    auto start = b.BeginBox("free");
    b.U32(0xDEADBEEF);  // 4 bytes payload
    b.EndBox(start);

    // size(4) + type(4) + payload(4) == 12
    EXPECT(b.buffer().size() == 12);
    EXPECT(b.buffer()[0] == 0x00 && b.buffer()[3] == 0x0C);
    EXPECT(std::memcmp(&b.buffer()[4], "free", 4) == 0);
    EXPECT(b.buffer()[8] == 0xDE);
}

void TestFullBox() {
    BoxBuilder b;
    auto start = b.BeginFullBox("tfdt", /*version=*/1, /*flags=*/0);
    b.U64(0x1234ULL);  // base_media_decode_time (v1)
    b.EndBox(start);

    // size(4) + type(4) + version(1) + flags(3) + payload(8) == 20
    EXPECT(b.buffer().size() == 20);
    EXPECT(b.buffer()[3] == 0x14);  // 20
    EXPECT(std::memcmp(&b.buffer()[4], "tfdt", 4) == 0);
    EXPECT(b.buffer()[8] == 0x01);  // version
    EXPECT(b.buffer()[9] == 0x00 && b.buffer()[10] == 0x00 && b.buffer()[11] == 0x00); // flags
    EXPECT(b.buffer()[19] == 0x34);
}

void TestNestedBoxes() {
    BoxBuilder b;
    auto outer = b.BeginBox("moof");
    {
        auto mfhd = b.BeginFullBox("mfhd", 0, 0);
        b.U32(42);  // sequence_number
        b.EndBox(mfhd);
    }
    b.EndBox(outer);

    // mfhd: size(4) + type(4) + verflags(4) + seq(4) = 16
    // moof: size(4) + type(4) + mfhd(16) = 24
    EXPECT(b.buffer().size() == 24);
    EXPECT(b.buffer()[3] == 0x18);  // outer size = 24
    EXPECT(std::memcmp(&b.buffer()[4], "moof", 4) == 0);
    EXPECT(b.buffer()[11] == 0x10);  // inner size = 16
    EXPECT(std::memcmp(&b.buffer()[12], "mfhd", 4) == 0);
}

void TestPatch32() {
    BoxBuilder b;
    b.U32(0);              // placeholder at offset 0
    size_t after_marker = b.Pos();
    b.U32(0xDEADBEEF);
    b.Patch32(0, static_cast<uint32_t>(after_marker));

    EXPECT(b.buffer().size() == 8);
    EXPECT(b.buffer()[0] == 0x00 && b.buffer()[3] == 0x04);  // patched to 4
    EXPECT(b.buffer()[4] == 0xDE);  // unaffected
}

void TestTake() {
    BoxBuilder b;
    b.U32(0xCAFE);
    auto v = b.Take();
    EXPECT(v.size() == 4);
    EXPECT(b.buffer().empty());  // moved out
}

}  // namespace

int main() {
    TestEndianWrites();
    TestSignedWrappers();
    TestLeafBox();
    TestFullBox();
    TestNestedBoxes();
    TestPatch32();
    TestTake();
    std::printf("test_box_builder: OK\n");
    return 0;
}
