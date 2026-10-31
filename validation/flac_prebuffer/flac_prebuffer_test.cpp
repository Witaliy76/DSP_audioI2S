// AF-2.5R legacy FLAC buffering contract (host).
// Production Audio.cpp no longer includes flac_startup.h.
// This fixture locks the restored b5aed2c / AF-2.5R predicates:
//   WebStream ready: filled > maxFrameSize || allData
//   WebFile ready:   filled > 2 * maxBlock
//   Decode gate:     readSpace >= maxBlock (unless lastFrames)
//
// Early unit-ready (AF-2.5) is historical only: helpers may still exist in
// flac_startup.h but must NOT be treated as the production PASS contract.
// Fill < 65536 with an open TCP remains OPEN (documented, not PASS).

#include "../../src/flac_startup.h"

#include <cstdio>
#include <initializer_list>

namespace {

int g_failed = 0;

void check(bool cond, const char* expr, int line) {
    if (cond) return;
    std::printf("FAIL %s:%d: %s\n", "flac_prebuffer_test.cpp", line, expr);
    ++g_failed;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void test_legacy_webstream() {
    const size_t flac_cap = 65535;
    const size_t mp3_cap = 18000;

    std::printf("LEGACY webstream filled>65535:");
    for (size_t n : {8192u, 16384u, 32768u, 65535u, 65536u, 131072u}) {
        const bool ready = audiolib::legacyWebStreamReady(n, flac_cap, false);
        std::printf(" %u=%d", static_cast<unsigned>(n), ready ? 1 : 0);
        if (n <= flac_cap) CHECK(!ready);
        if (n == 65536 || n == 131072) CHECK(ready);
    }
    std::printf("\n");

    // H02-class: 16 KiB during pause is not ready (waiting is expected).
    CHECK(!audiolib::legacyWebStreamReady(16384, flac_cap, false));

    // H03-class paced accumulation: ready only after capacity.
    CHECK(!audiolib::legacyWebStreamReady(32768, flac_cap, false));
    CHECK(audiolib::legacyWebStreamReady(65536, flac_cap, false));

    // Finished body may open without capacity.
    CHECK(audiolib::legacyWebStreamReady(100, flac_cap, true));

    // OPEN defect (not PASS): thrifty fill with open TCP, no allData.
    std::printf("OPEN fill<65536 open-TCP: not ready (expected legacy silence)\n");
    CHECK(!audiolib::legacyWebStreamReady(32768, flac_cap, false));

    // MP3/AAC numeric gate unchanged.
    CHECK(!audiolib::legacyWebStreamReady(18000, mp3_cap, false));
    CHECK(audiolib::legacyWebStreamReady(18001, mp3_cap, false));
}

void test_legacy_webfile() {
    const size_t flac_cap = 65535;
    std::printf("LEGACY webfile filled>131070:");
    for (size_t n : {65536u, 100000u, 131070u, 131071u}) {
        const bool ready = audiolib::legacyWebFileReady(n, flac_cap);
        std::printf(" %u=%d", static_cast<unsigned>(n), ready ? 1 : 0);
        if (n <= 2 * flac_cap) CHECK(!ready);
        if (n == 131071) CHECK(ready);
    }
    std::printf("\n");
}

void test_legacy_decode_gate() {
    const size_t flac_cap = 65535;
    const size_t mp3_cap = 18000;
    CHECK(!audiolib::legacyDecodeAllowed(65534, flac_cap, false));
    CHECK(audiolib::legacyDecodeAllowed(65535, flac_cap, false));
    CHECK(audiolib::legacyDecodeAllowed(100, flac_cap, true));
    CHECK(!audiolib::legacyDecodeAllowed(17999, mp3_cap, false));
    CHECK(audiolib::legacyDecodeAllowed(18000, mp3_cap, false));
}

void test_af25_early_ready_not_production() {
    // Historical AF-2.5 helper: one contained unit could open below capacity.
    // AF-2.5R production must NOT use this as stream-ready. Document only.
    const size_t unit = 70; // synthetic Ogg-page-sized stand-in
    const bool early = audiolib::webStreamStartupReady(true, unit + 16, 65535, false, unit);
    std::printf("HISTORICAL AF-2.5 early-ready helper returns %d (not production contract)\n", early ? 1 : 0);
    CHECK(early); // helper still compiles; production path ignores it
    CHECK(!audiolib::legacyWebStreamReady(unit + 16, 65535, false));
}

} // namespace

int main() {
    test_legacy_webstream();
    test_legacy_webfile();
    test_legacy_decode_gate();
    test_af25_early_ready_not_production();
    if (g_failed) {
        std::printf("flac_prebuffer: FAIL (%d)\n", g_failed);
        return 1;
    }
    std::printf("flac_prebuffer: PASS (legacy AF-2.5R contract)\n");
    return 0;
}
