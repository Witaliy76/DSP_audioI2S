// Host proof for the deferred Ogg last-segment buffer.
// Links the production VorbisDecoder (see run.ps1). The host ps_ptr::alloc
// places the requested bytes against a no-access page, so a write of
// capacity+1 faults before any later decoder work.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "vorbis_decoder.h"

namespace {

// Same contract as the allocation and both deferred-segment copies.
constexpr uint32_t kCap = VorbisDecoder::VORBIS_LAST_SEGMENT_CAPACITY;
static_assert(kCap == 4096, "AF-2.2 keeps the existing 4096-byte allocation");

constexpr int32_t kParseDone = VorbisDecoder::VORBIS_PARSE_OGG_DONE;
constexpr int32_t kErr = VorbisDecoder::VORBIS_ERR;
constexpr int32_t kBadPacket = -136;  // OV_EBADPACKET, synthesis of a non-setup packet

int g_fails = 0;

void fail(const char* name, const char* why) {
    std::printf("FAIL %s: %s\n", name, why);
    std::fflush(stdout);
    ++g_fails;
}

void append_page(std::vector<uint8_t>& out, uint8_t header_type, const std::vector<uint8_t>& lace,
                 const std::vector<uint8_t>& payload) {
    const size_t at = out.size();
    out.resize(at + 27 + lace.size() + payload.size());
    uint8_t* p = out.data() + at;
    std::memcpy(p, "OggS", 4);
    p[5] = header_type;
    p[26] = static_cast<uint8_t>(lace.size());
    if (!lace.empty()) { std::memcpy(p + 27, lace.data(), lace.size()); }
    if (!payload.empty()) { std::memcpy(p + 27 + lace.size(), payload.data(), payload.size()); }
}

// One finished Ogg packet: every lacing byte but the last is 255.
std::vector<uint8_t> lace_packet(int total) {
    std::vector<uint8_t> lace;
    int left = total;
    while (left > 255) {
        lace.push_back(255);
        left -= 255;
    }
    lace.push_back(static_cast<uint8_t>(left));
    return lace;
}

std::vector<uint8_t> lace_join(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Same grouping as VorbisDecoder::parse_OGG. Used only to check the fixture
// built the segment lengths it claims, not as a stand-in for the decoder.
std::vector<int> segments_from_lace(const std::vector<uint8_t>& lace) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(lace.size()); i++) {
        int n = lace[i];
        while (lace[i] == 255) {
            i++;
            if (i == static_cast<int>(lace.size())) { break; }
            n += lace[i];
        }
        out.push_back(n);
    }
    return out;
}

std::vector<uint8_t> audio_page(uint8_t header_type, const std::vector<uint8_t>& lace) {
    size_t nbytes = 0;
    for (uint8_t n : lace) { nbytes += n; }
    std::vector<uint8_t> payload(nbytes, 0x11);
    std::vector<uint8_t> page;
    append_page(page, header_type, lace, payload);
    return page;
}

std::vector<uint8_t> header_page(uint8_t packet_type, int payload_len) {
    std::vector<uint8_t> payload(payload_len, 0);
    payload[0] = packet_type;
    std::memcpy(payload.data() + 1, "vorbis", 6);
    std::vector<uint8_t> page;
    append_page(page, 0, {static_cast<uint8_t>(payload_len)}, payload);
    return page;
}

struct Step {
    int32_t ret;
    int32_t used;
};

std::vector<Step> drive(VorbisDecoder& dec, std::vector<uint8_t>& page, int32_t* pcm, int calls) {
    uint8_t* p = page.data();
    int32_t left = static_cast<int32_t>(page.size());
    std::vector<Step> steps;
    for (int i = 0; i < calls; ++i) {
        const int32_t before = left;
        std::printf("  call %d left=%d\n", i, left);
        std::fflush(stdout);
        const int32_t ret = dec.decode(p, &left, pcm);
        const int32_t used = before - left;
        std::printf("  call %d ret=%d used=%d\n", i, ret, used);
        std::fflush(stdout);
        steps.push_back({ret, used});
        if (used < 0) { break; }
        p += used;
        if (left <= 0) { break; }
    }
    return steps;
}

void reach_audio(VorbisDecoder& dec, int32_t* pcm) {
    std::vector<uint8_t> ident = header_page(1, 30);
    std::vector<uint8_t> comment = header_page(3, 32);
    std::vector<uint8_t> setup = header_page(5, 32);
    drive(dec, ident, pcm, 2);
    drive(dec, comment, pcm, 2);
    drive(dec, setup, pcm, 2);
}

bool direct_or_ok(int32_t ret) { return ret == 0 || ret == kBadPacket; }

void expect_parse_then(const char* name, const std::vector<Step>& steps, int index, int32_t want) {
    if (steps.empty() || steps[0].ret != kParseDone) {
        fail(name, "page was not parsed");
        return;
    }
    if (static_cast<int>(steps.size()) <= index) {
        fail(name, "decoder returned before the segment call");
        return;
    }
    if (steps[index].ret != want) {
        std::printf("FAIL %s: ret %d, expected %d\n", name, steps[index].ret, want);
        std::fflush(stdout);
        ++g_fails;
    }
}

void expect_in_range_copy(const char* name, const std::vector<Step>& steps, int index) {
    if (steps.empty() || steps[0].ret != kParseDone) {
        fail(name, "page was not parsed");
        return;
    }
    if (static_cast<int>(steps.size()) <= index) {
        fail(name, "decoder returned before the segment call");
        return;
    }
    if (steps[index].ret == kErr || !direct_or_ok(steps[index].ret)) {
        std::printf("FAIL %s: in-range copy ret %d\n", name, steps[index].ret);
        std::fflush(stdout);
        ++g_fails;
    }
}

struct Session {
    Audio audio;
    VorbisDecoder dec;
    std::vector<int32_t> pcm;
    explicit Session() : dec(audio), pcm(8192, 0) {
        if (!dec.init(9216)) { std::fprintf(stderr, "init failed\n"); }
    }
};

void case_defer(const char* name, int total, int32_t want) {
    std::printf("CASE %s defer %d\n", name, total);
    std::fflush(stdout);
    auto lace = lace_packet(total);
    auto segs = segments_from_lace(lace);
    if (lace.size() > 255 || segs.size() != 1 || segs[0] != total) {
        fail(name, "fixture lace is not one segment of the requested length");
        return;
    }
    Session s;
    reach_audio(s.dec, s.pcm.data());
    auto page = audio_page(0, lace);
    auto steps = drive(s.dec, page, s.pcm.data(), 2);
    expect_parse_then(name, steps, 1, want);
}

void case_noncontinued(const char* name, int first, int second) {
    std::printf("CASE %s non-continued %d + %d\n", name, first, second);
    std::fflush(stdout);
    auto lace = lace_join(lace_packet(first), lace_packet(second));
    auto segs = segments_from_lace(lace);
    if (lace.size() > 255 || segs.size() != 2 || segs[0] != first || segs[1] != second) {
        fail(name, "fixture lace is not two finished segments");
        return;
    }
    Session s;
    reach_audio(s.dec, s.pcm.data());
    auto page = audio_page(0, lace);
    auto steps = drive(s.dec, page, s.pcm.data(), 3);
    expect_in_range_copy(name, steps, 1);
    expect_parse_then(name, steps, 2, 0);
}

void case_continued(const char* name, int deferred, int addition, int32_t want_add) {
    std::printf("CASE %s continued defer %d append %d\n", name, deferred, addition);
    std::fflush(stdout);
    auto first_lace = lace_packet(deferred);
    auto second_lace = lace_join(lace_packet(addition), lace_packet(1));
    if (segments_from_lace(first_lace).size() != 1 || segments_from_lace(second_lace).size() != 2) {
        fail(name, "fixture continued page is not defer + finishing segment");
        return;
    }
    Session s;
    reach_audio(s.dec, s.pcm.data());
    auto page1 = audio_page(0, first_lace);
    auto primed = drive(s.dec, page1, s.pcm.data(), 2);
    const int fails_before = g_fails;
    expect_parse_then(name, primed, 1, 0);
    if (g_fails != fails_before) { return; }
    auto page2 = audio_page(0x01, second_lace);
    auto steps = drive(s.dec, page2, s.pcm.data(), 2);
    if (want_add == kErr) {
        expect_parse_then(name, steps, 1, kErr);
    } else {
        expect_in_range_copy(name, steps, 1);
    }
}

void case_stale_len(const char* name) {
    std::printf("CASE %s stale length\n", name);
    std::fflush(stdout);
    Session s;
    reach_audio(s.dec, s.pcm.data());

    auto small = audio_page(0, lace_packet(100));
    auto primed = drive(s.dec, small, s.pcm.data(), 2);
    expect_parse_then(name, primed, 1, 0);

    auto over = audio_page(0, lace_packet(static_cast<int>(kCap) + 1000));
    auto rejected = drive(s.dec, over, s.pcm.data(), 2);
    expect_parse_then(name, rejected, 1, kErr);

    // If the reject left the previous length of 100, appending kCap bytes
    // writes at offset 100 and crosses the guard page.
    auto probe_lace = lace_join(lace_packet(static_cast<int>(kCap)), lace_packet(1));
    auto probe = audio_page(0x01, probe_lace);
    std::printf("  probe append %u at whatever deferred length survived\n", kCap);
    std::fflush(stdout);
    auto steps = drive(s.dec, probe, s.pcm.data(), 2);
    expect_in_range_copy(name, steps, 1);
}

void case_sole_continued(const char* name) {
    std::printf("CASE %s sole continued segment %u\n", name, kCap + 1);
    std::fflush(stdout);
    auto lace = lace_packet(static_cast<int>(kCap) + 1);
    if (segments_from_lace(lace).size() != 1) {
        fail(name, "fixture lace is not one segment");
        return;
    }
    Session s;
    reach_audio(s.dec, s.pcm.data());
    auto page = audio_page(0x01, lace);
    auto steps = drive(s.dec, page, s.pcm.data(), 2);
    expect_parse_then(name, steps, 1, kErr);
}

struct Case {
    const char* name;
    void (*fn)();
};

void c_defer_under() { case_defer("defer_under", 100, 0); }
void c_defer_exact() { case_defer("defer_exact", static_cast<int>(kCap), 0); }
void c_defer_plus() { case_defer("defer_plus", static_cast<int>(kCap) + 1, kErr); }
void c_defer_4335() { case_defer("defer_4335", 17 * 255, kErr); }
void c_defer_huge() { case_defer("defer_huge", 255 * 255, kErr); }
void c_noncontinued() { case_noncontinued("noncontinued", 200, 40); }
void c_noncontinued_large() { case_noncontinued("noncontinued_large", 8000, 20); }
void c_cont_under() { case_continued("cont_under", 100, 50, 0); }
void c_cont_exact() { case_continued("cont_exact", 4000, static_cast<int>(kCap) - 4000, 0); }
void c_cont_plus() { case_continued("cont_plus", 4000, static_cast<int>(kCap) - 4000 + 1, kErr); }
void c_cont_huge() { case_continued("cont_huge", 100, 8000, kErr); }
void c_stale() { case_stale_len("stale_len"); }
void c_sole() { case_sole_continued("sole_continued_plus"); }

const Case kCases[] = {
    {"defer_under", c_defer_under},
    {"defer_exact", c_defer_exact},
    {"defer_plus", c_defer_plus},
    {"defer_4335", c_defer_4335},
    {"defer_huge", c_defer_huge},
    {"noncontinued", c_noncontinued},
    {"noncontinued_large", c_noncontinued_large},
    {"cont_under", c_cont_under},
    {"cont_exact", c_cont_exact},
    {"cont_plus", c_cont_plus},
    {"cont_huge", c_cont_huge},
    {"stale_len", c_stale},
    {"sole_continued_plus", c_sole},
};

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--list") == 0) {
        for (const Case& c : kCases) { std::printf("%s\n", c.name); }
        return 0;
    }
    std::printf("capacity %u max_lacing_segment %d\n", kCap, 255 * 255);
    std::fflush(stdout);
    const char* only = argc > 1 ? argv[1] : nullptr;
    bool ran = false;
    for (const Case& c : kCases) {
        if (only != nullptr && std::strcmp(only, c.name) != 0) { continue; }
        const int before = g_fails;
        ran = true;
        c.fn();
        if (g_fails == before) { std::printf("PASS %s\n", c.name); }
        std::fflush(stdout);
    }
    if (!ran) {
        std::fprintf(stderr, "unknown case\n");
        return 2;
    }
    if (g_fails != 0) {
        std::printf("vorbis_bounds: FAIL %d\n", g_fails);
        return 1;
    }
    std::printf("vorbis_bounds: PASS\n");
    return 0;
}
