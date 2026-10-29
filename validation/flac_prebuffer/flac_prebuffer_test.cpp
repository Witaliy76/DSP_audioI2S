// AF-2.5 / E-FL3 host fixture.
// Calls production src/flac_startup.h and the production FlacDecoder.
// The startup predicates are not reimplemented here.

#include "flac_decoder.h"
#include "../../src/flac_startup.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failed = 0;

void check(bool cond, const char* expr, int line) {
    if (cond) return;
    std::printf("FAIL %s:%d: %s\n", "flac_prebuffer_test.cpp", line, expr);
    ++g_failed;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

uint8_t crc8(const uint8_t* p, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc = static_cast<uint8_t>(crc ^ p[i]);
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

// Fixed-block native frame, mono, 16-bit, 44100 Hz, constant subframe.
// block_code 12 = 4096, 13 = 8192, 7 = explicit 16-bit block size.
std::vector<uint8_t> native_frame(uint8_t block_code, uint16_t explicit_block, uint8_t frame_nr, uint16_t sample) {
    std::vector<uint8_t> f;
    f.push_back(0xFF);
    f.push_back(0xF8);
    f.push_back(static_cast<uint8_t>((block_code << 4) | 0x09));
    f.push_back(static_cast<uint8_t>(0x04 << 1));
    f.push_back(frame_nr);
    if (block_code == 7) {
        const uint16_t coded = static_cast<uint16_t>(explicit_block - 1);
        f.push_back(static_cast<uint8_t>(coded >> 8));
        f.push_back(static_cast<uint8_t>(coded & 0xFF));
    }
    f.push_back(crc8(f.data(), f.size()));
    f.push_back(0x00);
    f.push_back(static_cast<uint8_t>(sample >> 8));
    f.push_back(static_cast<uint8_t>(sample & 0xFF));
    f.push_back(0x00);
    f.push_back(0x00);
    const uint16_t crc = audiolib::flac_startup_detail::crc16(f.data(), f.size() - 2);
    f[f.size() - 2] = static_cast<uint8_t>(crc >> 8);
    f[f.size() - 1] = static_cast<uint8_t>(crc & 0xFF);
    return f;
}

std::vector<uint8_t> streaminfo_block(uint16_t block) {
    std::vector<uint8_t> s(34, 0);
    s[0] = static_cast<uint8_t>(block >> 8);
    s[1] = static_cast<uint8_t>(block & 0xFF);
    s[2] = s[0];
    s[3] = s[1];
    const uint32_t rate = 44100;
    s[10] = static_cast<uint8_t>((rate >> 12) & 0xFF);
    s[11] = static_cast<uint8_t>((rate >> 4) & 0xFF);
    s[12] = static_cast<uint8_t>((rate & 0x0F) << 4);
    s[13] = 0xF0;
    return s;
}

std::vector<uint8_t> native_file(uint8_t block_code, uint16_t block, uint16_t sample) {
    std::vector<uint8_t> out = {'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 34};
    const auto           info = streaminfo_block(block);
    out.insert(out.end(), info.begin(), info.end());
    const auto a = native_frame(block_code, block, 0, sample);
    const auto b = native_frame(block_code, block, 1, sample);
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

std::vector<uint8_t> ogg_page(const std::vector<uint8_t>& payload, uint8_t header_type, uint32_t seq) {
    std::vector<uint8_t> p;
    const char           magic[] = {'O', 'g', 'g', 'S', 0};
    p.insert(p.end(), magic, magic + 5);
    p.push_back(header_type);
    for (int i = 0; i < 8; ++i) p.push_back(0);
    p.push_back(1);
    p.push_back(0);
    p.push_back(0);
    p.push_back(0);
    p.push_back(static_cast<uint8_t>(seq));
    p.push_back(static_cast<uint8_t>(seq >> 8));
    p.push_back(static_cast<uint8_t>(seq >> 16));
    p.push_back(static_cast<uint8_t>(seq >> 24));
    for (int i = 0; i < 4; ++i) p.push_back(0);
    p.push_back(1);
    p.push_back(static_cast<uint8_t>(payload.size()));
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

struct Feed {
    int res = 0;
    int used = 0;
    int samples = 0;
    int calls = 0;
};

Feed feed_native(FlacDecoder& dec, uint8_t* data, int len, int32_t* out, bool follow_contract, bool split_output = false) {
    Feed         r;
    const size_t window = follow_contract ? audiolib::flacDecodeWindow(data, static_cast<size_t>(len), split_output) : static_cast<size_t>(len);
    int32_t      left = static_cast<int32_t>(window);
    r.res = dec.decode(data, &left, out);
    if (left < 0 || static_cast<size_t>(left) > window) {
        r.used = -1;
        r.calls = 1;
        return r;
    }
    r.used = static_cast<int>(window) - left;
    r.calls = 1;
    r.samples = static_cast<int>(dec.getOutputSamples());
    if (!follow_contract) return r;
    const bool ogg = len >= 4 && data[0] == 'O' && data[1] == 'g' && data[2] == 'g' && data[3] == 'S';
    int        spins = 0;
    int        prev = -1;
    while (audiolib::flacDecodeContinue(r.res, r.used, left, window, spins, ogg, prev)) {
        prev = r.used;
        int32_t next = left;
        r.res = dec.decode(data + r.used, &next, out);
        if (next < 0 || next > left) {
            r.used = -1;
            break;
        }
        left = next;
        r.used = static_cast<int>(window) - left;
        r.samples = static_cast<int>(dec.getOutputSamples());
        ++r.calls;
        ++spins;
    }
    return r;
}

void expect_sample(const int32_t* out, int samples, uint16_t sample) {
    CHECK(samples > 0);
    const int32_t want = static_cast<int32_t>(sample) << 16;
    if (samples > 0) {
        CHECK(out[0] == want);
        CHECK(out[1] == want);
    }
}

void test_legacy_gates() {
    const size_t flac_cap = 65535;
    const size_t mp3_cap = 18000;
    std::printf("PRE-FIX webstream filled>65535:");
    for (size_t n : {8192u, 16384u, 32768u, 65535u, 65536u, 131072u}) {
        const bool ready = audiolib::legacyWebStreamReady(n, flac_cap, false);
        std::printf(" %u=%d", static_cast<unsigned>(n), ready ? 1 : 0);
        if (n <= flac_cap) CHECK(!ready);
        if (n == 65536) CHECK(ready);
    }
    std::printf("\n");
    CHECK(!audiolib::legacyWebStreamReady(0, 0, false));
    CHECK(audiolib::legacyWebStreamReady(1, 0, false));
    CHECK(audiolib::legacyWebStreamReady(100, flac_cap, true));

    std::printf("PRE-FIX webfile filled>131070:");
    for (size_t n : {65536u, 100000u, 131070u, 131071u}) {
        const bool ready = audiolib::legacyWebFileReady(n, flac_cap);
        std::printf(" %u=%d", static_cast<unsigned>(n), ready ? 1 : 0);
        if (n <= 2 * flac_cap) CHECK(!ready);
        if (n == 131071) CHECK(ready);
    }
    std::printf("\n");
    CHECK(!audiolib::legacyWebFileReady(100000, flac_cap));

    CHECK(!audiolib::legacyDecodeAllowed(65534, flac_cap, false));
    CHECK(audiolib::legacyDecodeAllowed(65535, flac_cap, false));
    CHECK(audiolib::legacyDecodeAllowed(100, flac_cap, true));
    CHECK(!audiolib::legacyDecodeAllowed(17999, mp3_cap, false));
    CHECK(audiolib::legacyDecodeAllowed(18000, mp3_cap, false));
    CHECK(!audiolib::legacyWebStreamReady(18000, mp3_cap, false));
    CHECK(audiolib::legacyWebStreamReady(18001, mp3_cap, false));
    std::printf("PRE-FIX no stall predicate: a 32768-byte FLAC fill stays not-ready with the socket open\n");
    CHECK(!audiolib::legacyWebStreamReady(32768, flac_cap, false));
}

void test_units_and_contract() {
    const auto     frame = native_frame(12, 4096, 0, 0x1234);
    const auto     next = native_frame(12, 4096, 1, 0x1234);
    std::vector<uint8_t> two = frame;
    two.insert(two.end(), next.begin(), next.end());
    CHECK(audiolib::flacContainedUnit(two.data(), two.size()) == frame.size());
    CHECK(audiolib::flacContainedUnit(two.data(), frame.size()) == frame.size());
    CHECK(audiolib::flacContainedUnit(two.data(), frame.size() - 1) == 0);
    CHECK(audiolib::flacContainedUnit(two.data(), frame.size() + 4) == 0);
    CHECK(audiolib::flacContainedUnit(frame.data(), frame.size()) == frame.size());
    {
        const uint8_t* digits = reinterpret_cast<const uint8_t*>("123456789");
        CHECK(audiolib::flac_startup_detail::crc16(digits, 9) == 0xFEE8);
        std::vector<uint8_t> saved = two;
        CHECK(audiolib::flacContainedUnit(two.data(), two.size()) == frame.size());
        CHECK(saved == two);
    }
    {
        std::vector<uint8_t> var = frame;
        var[1] = 0xF9;
        var[5] = crc8(var.data(), 5);
        std::vector<uint8_t> var_pair = var;
        var_pair.insert(var_pair.end(), next.begin(), next.end());
        CHECK(audiolib::flacContainedUnit(var.data(), var.size()) == 0);
        CHECK(audiolib::flacContainedUnit(var_pair.data(), var_pair.size()) == 0);
        CHECK(!audiolib::webStreamStartupReady(true, var_pair.size(), 65535, false, 0));
    }
    {
        std::vector<uint8_t> poisoned = frame;
        poisoned.pop_back();
        poisoned.pop_back();
        const auto fake = native_frame(12, 4096, 9, 0x00AB);
        poisoned.insert(poisoned.end(), fake.begin(), fake.begin() + 6);
        poisoned.push_back(0);
        poisoned.push_back(0);
        const uint16_t crc = audiolib::flac_startup_detail::crc16(poisoned.data(), poisoned.size() - 2);
        poisoned[poisoned.size() - 2] = static_cast<uint8_t>(crc >> 8);
        poisoned[poisoned.size() - 1] = static_cast<uint8_t>(crc & 0xFF);
        std::vector<uint8_t> with_next = poisoned;
        with_next.insert(with_next.end(), next.begin(), next.end());
        const size_t false_at = frame.size() - 2;
        CHECK(audiolib::flac_startup_detail::frameHeaderLen(with_next.data() + false_at, with_next.size() - false_at) != 0);
        CHECK(audiolib::flacContainedUnit(with_next.data(), with_next.size()) == poisoned.size());
        CHECK(audiolib::flacContainedUnit(with_next.data(), with_next.size()) != false_at);
        CHECK(audiolib::flacContainedUnit(poisoned.data(), false_at + 6) == 0);
    }
    {
        const size_t header = 6;
        std::vector<uint8_t> partial_next = frame;
        partial_next.insert(partial_next.end(), next.begin(), next.begin() + header);
        CHECK(audiolib::flacContainedUnit(partial_next.data(), partial_next.size()) == frame.size());
        CHECK(audiolib::flacContainedUnit(partial_next.data(), frame.size() + header - 1) == 0);
    }

    const auto file = native_file(12, 4096, 0x1234);
    const size_t file_unit = audiolib::flacContainedUnit(file.data(), file.size());
    CHECK(file_unit > frame.size());
    CHECK(file_unit < file.size());
    CHECK(file_unit < 65535);

    const auto f4608 = native_frame(7, 4608, 0, 0x0100);
    const auto n4608 = native_frame(7, 4608, 1, 0x0100);
    std::vector<uint8_t> pair4608 = f4608;
    pair4608.insert(pair4608.end(), n4608.begin(), n4608.end());
    CHECK(audiolib::flacContainedUnit(pair4608.data(), pair4608.size()) == f4608.size());

    const auto f8192 = native_frame(13, 8192, 0, 0x2222);
    const auto n8192 = native_frame(13, 8192, 1, 0x2222);
    std::vector<uint8_t> pair8192 = f8192;
    pair8192.insert(pair8192.end(), n8192.begin(), n8192.end());
    CHECK(audiolib::flacContainedUnit(pair8192.data(), pair8192.size()) == f8192.size());

    std::vector<uint8_t> meta = {'f', 'L', 'a', 'C', 0x00, 0x00, 0x00, 34};
    meta.resize(meta.size() + 20, 0);
    CHECK(audiolib::flacContainedUnit(meta.data(), meta.size()) == 0);

    std::vector<uint8_t> payload = {'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 34};
    payload.insert(payload.end(), 34, 0);
    const auto page = ogg_page(payload, 0x02, 0);
    CHECK(audiolib::flacContainedUnit(page.data(), page.size()) == page.size());
    CHECK(audiolib::flacContainedUnit(page.data(), page.size() - 1) == 0);
    CHECK(page.size() < 65535);
    std::vector<uint8_t> lace(255, 0x11);
    const auto           big = ogg_page(lace, 0, 1);
    CHECK(audiolib::flacContainedUnit(big.data(), big.size()) == 27u + 1u + 255u);
    CHECK(audiolib::flacContainedUnit(big.data(), 27) == 0);

    const size_t unit = frame.size();
    const size_t cap = 65535;
    CHECK(!audiolib::webStreamStartupReady(false, 200, 0, false, unit));
    CHECK(!audiolib::webStreamStartupReady(true, 8 * 1024, cap, false, 0));
    CHECK(!audiolib::webStreamStartupReady(true, 16 * 1024, cap, false, 0));
    CHECK(!audiolib::webStreamStartupReady(true, 32 * 1024, cap, false, 0));
    CHECK(!audiolib::webStreamStartupReady(true, 65535, cap, false, 0));
    CHECK(audiolib::webStreamStartupReady(true, 65536, cap, false, 0));
    CHECK(!audiolib::webStreamStartupReady(true, cap, cap, false, 0));
    CHECK(audiolib::webStreamStartupReady(true, cap + 1, cap, false, 0));
    CHECK(audiolib::webStreamStartupReady(true, unit + 16, cap, false, unit));
    CHECK(audiolib::webStreamStartupReady(true, 100, cap, true, 0));
    CHECK(!audiolib::webStreamStartupReady(true, 50, 0, false, 0));

    CHECK(!audiolib::webFileStartupReady(100000, cap, false, 0));
    CHECK(!audiolib::webFileStartupReady(131070, cap, false, 0));
    CHECK(audiolib::webFileStartupReady(131071, cap, false, 0));
    CHECK(audiolib::webFileStartupReady(unit + 32, cap, false, unit));
    CHECK(audiolib::webFileStartupReady(4000, cap, true, 0));
    CHECK(!audiolib::webFileStartupReady(30000, 18000, false, 0));
    CHECK(audiolib::webFileStartupReady(36001, 18000, false, 0));
    CHECK(audiolib::webFileStartupReady(9000, 18000, true, 0));

    CHECK(audiolib::decodeReadMinimum(two.size(), cap, unit) == unit);
    CHECK(audiolib::decodeReadMinimum(1000, cap, 0) == cap);
    CHECK(audiolib::decodeReadMinimum(18000, 18000, 0) == 18000);
    CHECK(unit < cap);
    CHECK(two.size() >= unit);
    CHECK(1000 < cap);

    CHECK(!audiolib::webStreamStartupReady(true, 18000, 18000, false, 0));
    CHECK(!audiolib::webStreamStartupReady(true, 10000, 18000, false, 0));
    CHECK(audiolib::webStreamStartupReady(true, 18001, 18000, false, 0));
    CHECK(!audiolib::webStreamStartupReady(false, 20000, 0, false, 0));
    CHECK(!audiolib::webStreamStartupReady(true, 20000, cap, false, 0));
    CHECK(audiolib::webStreamStartupReady(true, 20000, cap, false, unit));

    const uint32_t t0 = 1000;
    CHECK(!audiolib::startupStalled(t0, t0, true, true, audiolib::kStartupStallMs));
    CHECK(!audiolib::startupStalled(t0 + 7999, t0, true, false, audiolib::kStartupStallMs));
    CHECK(!audiolib::startupStalled(t0 + 8000, t0, true, false, audiolib::kStartupStallMs));
    CHECK(audiolib::startupStalled(t0 + 8001, t0, true, false, audiolib::kStartupStallMs));
    CHECK(!audiolib::startupStalled(t0 + 9000, t0 + 2000, true, true, audiolib::kStartupStallMs));
    CHECK(!audiolib::startupStalled(t0 + 9000, 0, false, false, audiolib::kStartupStallMs));
    const uint32_t restarted = t0 + 9000;
    CHECK(!audiolib::startupStalled(restarted + 8000, restarted, true, false, audiolib::kStartupStallMs));
    CHECK(audiolib::startupStalled(restarted + 8001, restarted, true, false, audiolib::kStartupStallMs));
    const bool stalled = audiolib::startupStalled(t0 + 8001, t0, true, false, audiolib::kStartupStallMs);
    CHECK(audiolib::startupStallApplies(true, false, false, stalled));
    CHECK(!audiolib::startupStallApplies(true, true, false, stalled));
    CHECK(!audiolib::startupStallApplies(true, false, true, stalled));
    CHECK(!audiolib::startupStallApplies(false, false, false, stalled));
    CHECK(audiolib::kStartupStallMs == 8000);
    std::printf("startup unit native=%u file=%u ogg_page=%u stall_ms=%u\n", static_cast<unsigned>(unit), static_cast<unsigned>(file_unit),
                static_cast<unsigned>(page.size()), audiolib::kStartupStallMs);
}

struct Played {
    size_t cursor = 0;
    int    samples = 0;
    int    frames = 0;
    bool   split = false;
    bool   stuck = false;
};

Played play_buf(FlacDecoder& dec, uint8_t* data, size_t len, int32_t* out) {
    Played r;
    int    guard = 0;
    while (r.cursor < len && guard < 32) {
        ++guard;
        uint8_t*   p = data + r.cursor;
        const size_t avail = len - r.cursor;
        if (!r.split) {
            const size_t unit = audiolib::flacContainedUnit(p, avail);
            if (unit == 0) break;
        }
        const size_t before = r.cursor;
        Feed         fed = feed_native(dec, p, static_cast<int>(avail), out, true, r.split);
        if (fed.used <= 0 || fed.calls > 8) {
            r.stuck = true;
            break;
        }
        CHECK(static_cast<size_t>(fed.used) <= avail);
        r.cursor += static_cast<size_t>(fed.used);
        CHECK(r.cursor > before);
        r.samples += fed.samples;
        if (fed.res == FlacDecoder::FLAC_NONE || fed.res == FlacDecoder::GIVE_NEXT_LOOP) {
            if (!r.split && fed.res != FlacDecoder::GIVE_NEXT_LOOP) ++r.frames;
            if (r.split && fed.res == FlacDecoder::FLAC_NONE) ++r.frames;
        }
        const bool next_split = (fed.res == FlacDecoder::GIVE_NEXT_LOOP);
        if (r.split && next_split) {
            r.stuck = true;
            break;
        }
        r.split = next_split;
    }
    return r;
}

void test_decoder() {
    Audio       audio;
    int32_t     out[9216];
    const auto  a = native_frame(12, 4096, 0, 0x1234);
    const auto  b = native_frame(12, 4096, 1, 0x1234);
    std::vector<uint8_t> two = a;
    two.insert(two.end(), b.begin(), b.end());

    {
        FlacDecoder dec(audio);
        CHECK(dec.init(9216));
        Feed one = feed_native(dec, two.data(), static_cast<int>(a.size()), out, false);
        std::printf("PRE-FIX decoder single call bytes=%u res=%d used=%d samples=%d calls=%d\n", static_cast<unsigned>(a.size()), one.res, one.used, one.samples, one.calls);
        CHECK(one.res == FlacDecoder::FLAC_DECODE_FRAMES_LOOP);
        CHECK(one.samples == 0);
        CHECK(one.used > 0);
        CHECK(one.used < static_cast<int>(a.size()));
    }
    {
        FlacDecoder dec(audio);
        CHECK(dec.init(9216));
        Feed fed = feed_native(dec, two.data(), static_cast<int>(two.size()), out, true);
        std::printf("native 4096 res=%d used=%d samples=%d calls=%d\n", fed.res, fed.used, fed.samples, fed.calls);
        CHECK(fed.calls == 2);
        CHECK(fed.res == FlacDecoder::FLAC_NONE);
        CHECK(fed.samples == 4096);
        CHECK(fed.used == static_cast<int>(a.size()));
        expect_sample(out, fed.samples, 0x1234);
        int32_t left = static_cast<int32_t>(two.size() - fed.used);
        const int res2 = dec.decode(two.data() + fed.used, &left, out);
        CHECK(res2 == FlacDecoder::FLAC_DECODE_FRAMES_LOOP || res2 == FlacDecoder::FLAC_NONE);
        if (res2 == FlacDecoder::FLAC_DECODE_FRAMES_LOOP) {
            int32_t left2 = left;
            const int res3 = dec.decode(two.data() + (two.size() - left), &left2, out);
            CHECK(res3 == FlacDecoder::FLAC_NONE);
            CHECK(dec.getOutputSamples() == 4096);
            expect_sample(out, 4096, 0x1234);
        } else {
            CHECK(dec.getOutputSamples() == 4096);
        }
    }
    {
        std::vector<uint8_t> padded = a;
        padded.resize(30000, 0);
        FlacDecoder dec(audio);
        CHECK(dec.init(9216));
        int32_t left = static_cast<int32_t>(padded.size());
        const int res = dec.decode(padded.data(), &left, out);
        const int samples = static_cast<int>(dec.getOutputSamples());
        std::printf("native padded30000 res=%d samples=%d left=%d\n", res, samples, left);
        CHECK(res == FlacDecoder::FLAC_NONE);
        CHECK(samples == 4096);
        expect_sample(out, samples, 0x1234);
    }
    {
        const auto fa = native_frame(7, 4608, 0, 0x0100);
        const auto fb = native_frame(7, 4608, 1, 0x0100);
        std::vector<uint8_t> pair = fa;
        pair.insert(pair.end(), fb.begin(), fb.end());
        FlacDecoder dec(audio);
        CHECK(dec.init(9216));
        Feed fed = feed_native(dec, pair.data(), static_cast<int>(pair.size()), out, true);
        std::printf("native 4608 res=%d samples=%d calls=%d\n", fed.res, fed.samples, fed.calls);
        CHECK(fed.res == FlacDecoder::FLAC_NONE);
        CHECK(fed.samples == 4608);
        expect_sample(out, fed.samples, 0x0100);
    }
    {
        const auto fa = native_frame(13, 8192, 0, 0x2222);
        const auto fb = native_frame(13, 8192, 1, 0x2222);
        std::vector<uint8_t> pair = fa;
        pair.insert(pair.end(), fb.begin(), fb.end());
        FlacDecoder dec(audio);
        CHECK(dec.init(9216));
        Feed fed = feed_native(dec, pair.data(), static_cast<int>(pair.size()), out, true);
        std::printf("native 8192 first res=%d used=%d samples=%d calls=%d\n", fed.res, fed.used, fed.samples, fed.calls);
        CHECK(fed.res == FlacDecoder::GIVE_NEXT_LOOP);
        CHECK(fed.samples == 4608);
        CHECK(fed.used > 0);
        CHECK(fed.used < static_cast<int>(fa.size()));
        CHECK(fed.calls <= 8);
        expect_sample(out, fed.samples, 0x2222);
        const bool split = true;
        Feed       rest = feed_native(dec, pair.data() + fed.used, static_cast<int>(pair.size() - fed.used), out, true, split);
        std::printf("native 8192 rest res=%d used=%d samples=%d\n", rest.res, rest.used, rest.samples);
        CHECK(rest.res == FlacDecoder::FLAC_NONE);
        CHECK(rest.samples == 8192 - 4608);
        CHECK(fed.samples + rest.samples == 8192);
        CHECK(fed.used + rest.used == static_cast<int>(fa.size()));
        CHECK(!split || rest.res != FlacDecoder::GIVE_NEXT_LOOP);
        expect_sample(out, rest.samples, 0x2222);
        Feed nextf = feed_native(dec, pair.data() + fa.size(), static_cast<int>(fb.size()), out, true);
        std::printf("native 8192 next res=%d used=%d samples=%d\n", nextf.res, nextf.used, nextf.samples);
        CHECK(nextf.res == FlacDecoder::GIVE_NEXT_LOOP);
        CHECK(nextf.samples == 4608);
        Feed next_rest = feed_native(dec, pair.data() + fa.size() + nextf.used, static_cast<int>(fb.size() - nextf.used), out, true, true);
        CHECK(next_rest.res == FlacDecoder::FLAC_NONE);
        CHECK(next_rest.samples == 8192 - 4608);
        CHECK(nextf.used + next_rest.used == static_cast<int>(fb.size()));
        expect_sample(out, next_rest.samples, 0x2222);
    }
    {
        const auto f4096 = native_frame(12, 4096, 0, 0x1111);
        const auto f8192 = native_frame(13, 8192, 1, 0x2222);
        const auto f4096b = native_frame(12, 4096, 2, 0x3333);
        const auto f4608 = native_frame(7, 4608, 3, 0x0100);
        CHECK(f4096.size() != f4608.size());
        const int expect = 4096 + 8192 + 4608 + 4096;
        std::vector<uint8_t> seq = f4096;
        seq.insert(seq.end(), f8192.begin(), f8192.end());
        seq.insert(seq.end(), f4608.begin(), f4608.end());
        seq.insert(seq.end(), f4096b.begin(), f4096b.end());
        std::vector<uint8_t> grown;
        FlacDecoder          dec(audio);
        CHECK(dec.init(9216));
        size_t produced = 0;
        bool   saw_partial = false;
        int    chunk_samples = 0;
        for (size_t i = 0; i < seq.size(); i += 4) {
            const size_t n = (seq.size() - i) < 4 ? (seq.size() - i) : 4;
            grown.insert(grown.end(), seq.begin() + static_cast<std::ptrdiff_t>(i), seq.begin() + static_cast<std::ptrdiff_t>(i + n));
            Played part = play_buf(dec, grown.data() + produced, grown.size() - produced, out);
            CHECK(!part.stuck);
            CHECK(!part.split);
            if (part.cursor == 0 && grown.size() > produced) saw_partial = true;
            produced += part.cursor;
            chunk_samples += part.samples;
            CHECK(produced <= grown.size());
        }
        CHECK(saw_partial);
        CHECK(produced == grown.size());
        CHECK(chunk_samples == expect);
        FlacDecoder dec2(audio);
        CHECK(dec2.init(9216));
        Played all = play_buf(dec2, seq.data(), seq.size(), out);
        std::printf("sequence cursor=%u/%u samples=%d frames=%d stuck=%d split=%d partial=%d lens=%u,%u,%u,%u\n", static_cast<unsigned>(all.cursor),
                    static_cast<unsigned>(seq.size()), all.samples, all.frames, all.stuck ? 1 : 0, all.split ? 1 : 0, saw_partial ? 1 : 0,
                    static_cast<unsigned>(f4096.size()), static_cast<unsigned>(f8192.size()), static_cast<unsigned>(f4608.size()), static_cast<unsigned>(f4096b.size()));
        CHECK(!all.stuck);
        CHECK(!all.split);
        CHECK(all.cursor == seq.size());
        CHECK(all.samples == expect);
        CHECK(all.frames == 4);
        CHECK(audiolib::flacContainedUnit(seq.data(), seq.size()) == f4096.size());
        CHECK(audiolib::flacContainedUnit(seq.data(), seq.size()) < 65535);
        CHECK(!audiolib::webStreamStartupReady(true, f4096.size() - 1, 65535, false, 0));
    }
    {
        std::vector<uint8_t> id = {'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 34};
        const auto           info = streaminfo_block(4096);
        id.insert(id.end(), info.begin(), info.end());
        auto                 page0 = ogg_page(id, 0x02, 0);
        const auto           frame = native_frame(12, 4096, 0, 0x1234);
        auto                 page1 = ogg_page(frame, 0x00, 1);
        std::vector<uint8_t> both = page0;
        both.insert(both.end(), page1.begin(), page1.end());
        CHECK(audiolib::flacContainedUnit(both.data(), both.size()) == page0.size());
        CHECK(page0.size() < 65535);
        CHECK(page1.size() < 65535);

        FlacDecoder dec(audio);
        CHECK(dec.init(9216));
        Feed meta = feed_native(dec, both.data(), static_cast<int>(both.size()), out, true);
        std::printf("ogg metadata res=%d used=%d samples=%d calls=%d page=%u\n", meta.res, meta.used, meta.samples, meta.calls, static_cast<unsigned>(page0.size()));
        CHECK(meta.used == static_cast<int>(page0.size()));
        CHECK(meta.samples == 0);
        CHECK(meta.res == FlacDecoder::FLAC_PARSE_OGG_DONE);

        Feed audio_page = feed_native(dec, both.data() + meta.used, static_cast<int>(both.size() - meta.used), out, true);
        std::printf("ogg audio res=%d used=%d samples=%d calls=%d\n", audio_page.res, audio_page.used, audio_page.samples, audio_page.calls);
        CHECK(audio_page.samples == 4096);
        CHECK(audio_page.res == FlacDecoder::FLAC_NONE || audio_page.res == FlacDecoder::GIVE_NEXT_LOOP);
        expect_sample(out, audio_page.samples, 0x1234);
        CHECK(audio_page.calls <= 8);
        CHECK(meta.used + audio_page.used == static_cast<int>(both.size()));
    }
    {
        const auto           frame = native_frame(12, 4096, 3, 0x1234);
        std::vector<uint8_t> lace = {5, static_cast<uint8_t>(frame.size() - 5)};
        std::vector<uint8_t> page;
        const char           magic[] = {'O', 'g', 'g', 'S', 0, 0};
        page.insert(page.end(), magic, magic + 6);
        for (int i = 0; i < 20; ++i) page.push_back(0);
        page.push_back(static_cast<uint8_t>(lace.size()));
        page.insert(page.end(), lace.begin(), lace.end());
        page.insert(page.end(), frame.begin(), frame.end());
        CHECK(audiolib::flacContainedUnit(page.data(), page.size()) == page.size());
        CHECK(audiolib::flacContainedUnit(page.data(), page.size() - 1) == 0);
        std::vector<uint8_t> short_lace = page;
        short_lace[26] = 4;
        CHECK(audiolib::flacContainedUnit(short_lace.data(), 27) == 0);
        CHECK(audiolib::flacContainedUnit(short_lace.data(), 30) == 0);

        std::vector<uint8_t> ident = {0x7F, 'F', 'L', 'A', 'C', 1, 0, 0, 1};
        ident.push_back(0x80);
        ident.push_back(0x00);
        ident.push_back(0x00);
        ident.push_back(34);
        const auto info = streaminfo_block(4096);
        ident.insert(ident.end(), info.begin(), info.end());
        auto                 real = ogg_page(ident, 0x02, 0);
        auto                 audio_page = ogg_page(frame, 0x00, 1);
        std::vector<uint8_t> real_both = real;
        real_both.insert(real_both.end(), audio_page.begin(), audio_page.end());
        CHECK(audiolib::flacContainedUnit(real_both.data(), real_both.size()) == real.size());
        CHECK(std::memcmp(ident.data() + 1, "FLAC", 4) == 0);
        FlacDecoder dec(audio);
        CHECK(dec.init(9216));
        Feed mapped = feed_native(dec, real_both.data(), static_cast<int>(real_both.size()), out, true);
        std::printf("ogg 0x7F FLAC res=%d used=%d samples=%d calls=%d page=%u\n", mapped.res, mapped.used, mapped.samples, mapped.calls, static_cast<unsigned>(real.size()));
        CHECK(mapped.samples == 0);
        CHECK(mapped.used > 0);
        CHECK(mapped.used <= static_cast<int>(real.size()));
        CHECK(mapped.calls <= 8);
    }
}

} // namespace

int main() {
    test_legacy_gates();
    test_units_and_contract();
    test_decoder();
    if (g_failed) {
        std::printf("flac_prebuffer: FAIL %d\n", g_failed);
        return 1;
    }
    std::printf("flac_prebuffer: PASS\n");
    return 0;
}
