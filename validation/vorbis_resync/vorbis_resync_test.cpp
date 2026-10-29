// AF-2.4. Compiles production src/vorbis_decoder. The friend probe only reads state.
#include "vorbis_decoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct VorbisSetupProbe {
    static uint8_t  page(const VorbisDecoder& d) { return d.m_pageNr; }
    static bool     dsp(const VorbisDecoder& d) { return d.m_dsp_state.valid(); }
    static bool     continued(const VorbisDecoder& d) { return d.m_f_oggContinuedPage; }
    static bool     first(const VorbisDecoder& d) { return d.m_f_oggFirstPage; }
    static bool     last_page(const VorbisDecoder& d) { return d.m_f_oggLastPage; }
    static uint32_t deferred(const VorbisDecoder& d) { return d.m_ogg_items.lastSegmentTableLen; }
    static size_t   segments(const VorbisDecoder& d) { return d.m_ogg_items.segment_table.size(); }
    static uint32_t segment0(const VorbisDecoder& d) {
        return d.m_ogg_items.segment_table.empty() ? 0u : d.m_ogg_items.segment_table.front();
    }
    static bool bit_live(const VorbisDecoder& d) { return d.m_bitReader.data != nullptr; }
};

static int g_fails = 0;

struct Session {
    Audio         audio;
    VorbisDecoder dec;
    explicit Session() : dec(audio) { dec.init(9216); }
};

static void expect(bool cond, const char* name, const std::string& detail) {
    if (cond) {
        std::printf("  PASS  %s  %s\n", name, detail.c_str());
        return;
    }
    std::printf("  FAIL  %s  %s\n", name, detail.c_str());
    ++g_fails;
}

struct Bits {
    std::vector<uint8_t> bytes;
    int                  filled = 0;
    void                 put(uint32_t value, int n) {
        for (int i = 0; i < n; ++i) {
            if (bytes.empty() || filled == 8) {
                bytes.push_back(0);
                filled = 0;
            }
            if (value & 1u) { bytes.back() = static_cast<uint8_t>(bytes.back() | (1u << filled)); }
            value >>= 1;
            ++filled;
        }
    }
};

static std::vector<uint8_t> valid_setup_payload() {
    Bits w;
    w.put(0, 8);
    w.put(0x564342, 24);
    w.put(1, 16);
    w.put(7, 24);
    w.put(0, 1);
    w.put(1, 1);
    w.put(1, 1);
    w.put(0, 5);
    for (int i = 0; i < 6; ++i) { w.put(0, 1); }
    w.put(0, 4);
    w.put(0, 6);
    w.put(0, 16);
    w.put(0, 6);
    w.put(0, 16);
    w.put(1, 8);
    w.put(1, 16);
    w.put(1, 16);
    w.put(0, 6);
    w.put(0, 8);
    w.put(0, 4);
    w.put(0, 8);
    w.put(0, 6);
    w.put(0, 16);
    w.put(0, 24);
    w.put(0, 24);
    w.put(0, 24);
    w.put(0, 6);
    w.put(0, 8);
    w.put(0, 3);
    w.put(0, 1);
    w.put(0, 6);
    w.put(0, 16);
    w.put(1, 1);
    w.put(0, 4);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 8);
    w.put(0, 8);
    w.put(0, 8);
    w.put(0, 6);
    w.put(0, 1);
    w.put(0, 16);
    w.put(0, 16);
    w.put(0, 8);
    w.put(1, 1);
    std::vector<uint8_t> payload(7 + w.bytes.size(), 0);
    payload[0] = 5;
    std::memcpy(payload.data() + 1, "vorbis", 6);
    std::memcpy(payload.data() + 7, w.bytes.data(), w.bytes.size());
    return payload;
}

static void append_page(std::vector<uint8_t>& out, uint8_t header_type, const std::vector<uint8_t>& lace, const std::vector<uint8_t>& payload) {
    const size_t at = out.size();
    out.resize(at + 27 + lace.size() + payload.size());
    uint8_t* p = out.data() + at;
    std::memcpy(p, "OggS", 4);
    p[5] = header_type;
    p[26] = static_cast<uint8_t>(lace.size());
    if (!lace.empty()) { std::memcpy(p + 27, lace.data(), lace.size()); }
    if (!payload.empty()) { std::memcpy(p + 27 + lace.size(), payload.data(), payload.size()); }
}

static std::vector<uint8_t> wrap_packet(uint8_t header_type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> page;
    append_page(page, header_type, {static_cast<uint8_t>(payload.size())}, payload);
    return page;
}

static std::vector<uint8_t> lace_packet(int total) {
    std::vector<uint8_t> lace;
    int                  left = total;
    while (left > 255) {
        lace.push_back(255);
        left -= 255;
    }
    lace.push_back(static_cast<uint8_t>(left));
    return lace;
}

static std::vector<uint8_t> filled_page(uint8_t header_type, const std::vector<uint8_t>& lace, uint8_t fill) {
    size_t nbytes = 0;
    for (uint8_t n : lace) { nbytes += n; }
    std::vector<uint8_t> payload(nbytes, fill);
    std::vector<uint8_t> page;
    append_page(page, header_type, lace, payload);
    return page;
}

static std::vector<uint8_t> ident_payload() {
    std::vector<uint8_t> p(30, 0);
    p[0] = 1;
    std::memcpy(p.data() + 1, "vorbis", 6);
    p[11] = 1;
    p[12] = 0x44;
    p[13] = 0xAC;
    p[28] = 0x66;
    return p;
}

static std::vector<uint8_t> comment_payload() {
    std::vector<uint8_t> p(32, 0);
    p[0] = 3;
    std::memcpy(p.data() + 1, "vorbis", 6);
    return p;
}

static std::vector<uint8_t> empty_page(uint8_t header_type) {
    std::vector<uint8_t> page(27, 0);
    std::memcpy(page.data(), "OggS", 4);
    page[5] = header_type;
    return page;
}

struct Step {
    int32_t ret = 0;
    int32_t used = 0;
};

struct Cursor {
    std::vector<uint8_t> buf;
    size_t               off = 0;
    int32_t              pcm[8192]{};

    Step pull(VorbisDecoder& dec) {
        int32_t left = static_cast<int32_t>(buf.size() - off);
        const int32_t before = left;
        const int32_t ret = dec.decode(buf.data() + off, &left, pcm);
        const int32_t used = before - left;
        if (used > 0) { off += static_cast<size_t>(used); }
        return Step{ret, used};
    }
};

static std::string snap(const VorbisDecoder& dec, Step s) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "ret=%d used=%d page=%u cont=%d first=%d last=%d def=%u segs=%u seg0=%u bit=%d dsp=%d", s.ret, s.used,
                  VorbisSetupProbe::page(dec), VorbisSetupProbe::continued(dec) ? 1 : 0, VorbisSetupProbe::first(dec) ? 1 : 0,
                  VorbisSetupProbe::last_page(dec) ? 1 : 0, VorbisSetupProbe::deferred(dec), static_cast<unsigned>(VorbisSetupProbe::segments(dec)),
                  VorbisSetupProbe::segment0(dec), VorbisSetupProbe::bit_live(dec) ? 1 : 0, VorbisSetupProbe::dsp(dec) ? 1 : 0);
    return buf;
}

static bool feed_chain(VorbisDecoder& dec, uint8_t ident_type) {
    Cursor ident{wrap_packet(ident_type, ident_payload())};
    const Step id_parse = ident.pull(dec);
    const Step id_body = ident.pull(dec);
    Cursor comment{wrap_packet(0x00, comment_payload())};
    const Step co_parse = comment.pull(dec);
    const Step co_body = comment.pull(dec);
    Cursor setup{wrap_packet(0x00, valid_setup_payload())};
    const Step su_parse = setup.pull(dec);
    const Step su_body = setup.pull(dec);
    const bool ok = id_parse.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && id_body.ret >= 0 && co_parse.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && co_body.ret >= 0 &&
                    su_parse.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && su_body.ret >= 0 && VorbisSetupProbe::page(dec) == 4 && VorbisSetupProbe::dsp(dec) &&
                    dec.getChannels() == 1 && dec.getSampleRate() == 44100;
    if (!ok) {
        std::printf("    chain ident %s\n    chain comment %s\n    chain setup %s\n", snap(dec, id_body).c_str(), snap(dec, co_body).c_str(), snap(dec, su_body).c_str());
    }
    return ok;
}

static bool reach_audio(VorbisDecoder& dec) { return feed_chain(dec, 0x02); }

static std::vector<uint8_t> audio_pair() {
    std::vector<uint8_t> page;
    append_page(page, 0x00, {1, 1}, {0x00, 0x00});
    return page;
}

// 8192 bytes with no OggS. Byte 25 is 1 and byte 26 is 9: a one-byte-backward
// parse would publish a single segment of length 9. A guard byte sits in front.
static std::vector<uint8_t> missing_sync_window() {
    std::vector<uint8_t> mem(1 + 8192, 0);
    mem[0] = 0xA5;
    mem[1 + 25] = 1;
    mem[1 + 26] = 9;
    return mem;
}

static Step decode_at(VorbisDecoder& dec, uint8_t* p, int32_t n, int32_t* pcm) {
    int32_t left = n;
    const int32_t ret = dec.decode(p, &left, pcm);
    return Step{ret, n - left};
}

static void test_plain_and_audio() {
    Session session;
    const bool chain = reach_audio(session.dec);
    expect(chain, "ident_comment_setup", chain ? "page=4 dsp=1" : "chain failed");
    Cursor audio{audio_pair()};
    const Step parsed = audio.pull(session.dec);
    const Step body = audio.pull(session.dec);
    const bool ok = chain && parsed.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && body.ret == 0 && body.used == 1 && VorbisSetupProbe::page(session.dec) == 4 &&
                    VorbisSetupProbe::dsp(session.dec) && VorbisSetupProbe::deferred(session.dec) == 0;
    expect(ok, "first_audio", snap(session.dec, body));
}

static void test_continuation() {
    Session session;
    if (!reach_audio(session.dec)) {
        expect(false, "cont_reach", "chain failed");
        return;
    }
    Cursor first{filled_page(0x00, lace_packet(100), 0x11)};
    const Step parsed = first.pull(session.dec);
    const Step saved = first.pull(session.dec);
    const bool primed = parsed.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && saved.ret == 0 && VorbisSetupProbe::deferred(session.dec) == 100 &&
                        VorbisSetupProbe::segments(session.dec) == 0;
    expect(primed, "cont_deferred_saved", snap(session.dec, saved));

    std::vector<uint8_t> lace = lace_packet(50);
    lace.push_back(1);
    Cursor second{filled_page(0x01, lace, 0x11)};
    const Step cont_parse = second.pull(session.dec);
    const bool kept = cont_parse.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && VorbisSetupProbe::continued(session.dec) &&
                      VorbisSetupProbe::deferred(session.dec) == 100 && VorbisSetupProbe::segments(session.dec) == 2;
    expect(kept, "cont_length_survives_parse", snap(session.dec, cont_parse));
    const Step merged = second.pull(session.dec);
    const bool joined = merged.ret == 0 && merged.used == 50 && !VorbisSetupProbe::continued(session.dec) && VorbisSetupProbe::deferred(session.dec) == 0;
    expect(joined, "cont_merged", snap(session.dec, merged));
}

static void test_continuation_capacity() {
    Session session;
    if (!reach_audio(session.dec)) {
        expect(false, "cont_cap_reach", "chain failed");
        return;
    }
    Cursor first{filled_page(0x00, lace_packet(4000), 0x11)};
    first.pull(session.dec);
    const Step saved = first.pull(session.dec);
    expect(saved.ret == 0 && VorbisSetupProbe::deferred(session.dec) == 4000, "cont_cap_saved", snap(session.dec, saved));
    std::vector<uint8_t> lace = lace_packet(97);
    lace.push_back(1);
    Cursor second{filled_page(0x01, lace, 0x11)};
    second.pull(session.dec);
    const bool still = VorbisSetupProbe::deferred(session.dec) == 4000 && VorbisSetupProbe::continued(session.dec);
    const Step rejected = second.pull(session.dec);
    const bool ok = still && rejected.ret == VorbisDecoder::VORBIS_ERR && VorbisSetupProbe::deferred(session.dec) == 0 &&
                    VorbisSetupProbe::segments(session.dec) == 0 && !VorbisSetupProbe::continued(session.dec);
    expect(ok, "cont_oversize_rejects", snap(session.dec, rejected));
}

static void test_noncontinued() {
    Session session;
    if (!reach_audio(session.dec)) {
        expect(false, "noncont_reach", "chain failed");
        return;
    }
    Cursor first{filled_page(0x00, lace_packet(100), 0x11)};
    first.pull(session.dec);
    first.pull(session.dec);
    std::vector<uint8_t> lace = lace_packet(40);
    lace.push_back(1);
    Cursor second{filled_page(0x00, lace, 0x11)};
    const Step parsed = second.pull(session.dec);
    const bool kept = parsed.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && !VorbisSetupProbe::continued(session.dec) &&
                      VorbisSetupProbe::deferred(session.dec) == 100;
    expect(kept, "noncont_keeps_deferred", snap(session.dec, parsed));
    const Step played = second.pull(session.dec);
    const bool ok = played.ret == 0 && VorbisSetupProbe::deferred(session.dec) == 0 && VorbisSetupProbe::dsp(session.dec);
    expect(ok, "noncont_plays_deferred", snap(session.dec, played));
}

static void test_missing_oggs() {
    Session session;
    auto mem = missing_sync_window();
    int32_t pcm[8192]{};
    const Step s = decode_at(session.dec, mem.data() + 1, 8192, pcm);
    const bool shifted = s.ret >= 0 && VorbisSetupProbe::segments(session.dec) == 1 && VorbisSetupProbe::segment0(session.dec) == 9;
    const bool ok = s.ret == VorbisDecoder::VORBIS_ERR && s.used == 0 && VorbisSetupProbe::segments(session.dec) == 0 &&
                    VorbisSetupProbe::deferred(session.dec) == 0 && !VorbisSetupProbe::continued(session.dec) && !shifted && mem[0] == 0xA5;
    expect(ok, "missing_oggs", snap(session.dec, s) + (shifted ? " SHIFTED" : ""));
}

static void test_stale_after_defer() {
    Session session;
    if (!reach_audio(session.dec)) {
        expect(false, "stale_reach", "chain failed");
        return;
    }
    Cursor first{filled_page(0x00, lace_packet(100), 0x11)};
    first.pull(session.dec);
    const Step saved = first.pull(session.dec);
    expect(saved.ret == 0 && VorbisSetupProbe::deferred(session.dec) == 100, "stale_saved", snap(session.dec, saved));
    auto mem = missing_sync_window();
    const Step bad = decode_at(session.dec, mem.data() + 1, 8192, first.pcm);
    const bool cleared = bad.ret == VorbisDecoder::VORBIS_ERR && bad.used == 0 && VorbisSetupProbe::deferred(session.dec) == 0 &&
                         VorbisSetupProbe::segments(session.dec) == 0 && !VorbisSetupProbe::continued(session.dec) && !VorbisSetupProbe::last_page(session.dec);
    expect(cleared, "defer_then_missing_sync", snap(session.dec, bad));

    Cursor again{filled_page(0x00, lace_packet(40), 0x11)};
    const Step parsed = again.pull(session.dec);
    const bool fresh = cleared && parsed.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && VorbisSetupProbe::deferred(session.dec) == 0 &&
                       VorbisSetupProbe::segments(session.dec) == 1 && VorbisSetupProbe::dsp(session.dec);
    expect(fresh, "next_page_not_stale_packet", snap(session.dec, parsed));
}

static void test_continued_same_offset() {
    Session session;
    if (!reach_audio(session.dec)) {
        expect(false, "same_off_reach", "chain failed");
        return;
    }
    Cursor marker{filled_page(0x00, lace_packet(80), 0x11)};
    marker.pull(session.dec);
    marker.pull(session.dec);
    Cursor primed{empty_page(0x01)};
    const Step prime = primed.pull(session.dec);
    const bool armed = prime.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && VorbisSetupProbe::continued(session.dec) &&
                       VorbisSetupProbe::deferred(session.dec) == 80 && VorbisSetupProbe::segments(session.dec) == 0;
    expect(armed, "continued_armed", snap(session.dec, prime));

    std::vector<uint8_t> buf(8, 0x5A);
    auto page = filled_page(0x00, lace_packet(16), 0x11);
    buf.insert(buf.end(), page.begin(), page.end());
    const Step first = decode_at(session.dec, buf.data(), static_cast<int32_t>(buf.size()), primed.pcm);
    const bool blocked = first.ret == VorbisDecoder::VORBIS_ERR && first.used == 0 && !VorbisSetupProbe::continued(session.dec) &&
                         VorbisSetupProbe::deferred(session.dec) == 0 && VorbisSetupProbe::segments(session.dec) == 0;
    expect(blocked, "continued_error_clears", snap(session.dec, first));
    const Step second = decode_at(session.dec, buf.data(), static_cast<int32_t>(buf.size()), primed.pcm);
    const bool progressed = second.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && second.used > 8 && VorbisSetupProbe::segments(session.dec) == 1 &&
                            VorbisSetupProbe::deferred(session.dec) == 0;
    expect(progressed, "same_offset_then_resync", snap(session.dec, second));
}

static void test_bos_after_audio() {
    Session session;
    if (!reach_audio(session.dec)) {
        expect(false, "bos_reach", "chain failed");
        return;
    }
    const bool live = VorbisSetupProbe::bit_live(session.dec);
    Cursor saved{filled_page(0x00, lace_packet(100), 0x11)};
    saved.pull(session.dec);
    saved.pull(session.dec);
    expect(live && VorbisSetupProbe::deferred(session.dec) == 100 && VorbisSetupProbe::page(session.dec) == 4, "bos_primed",
           snap(session.dec, Step{}));
    Cursor bos{wrap_packet(0x02, ident_payload())};
    const Step parsed = bos.pull(session.dec);
    const bool clean = parsed.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && VorbisSetupProbe::page(session.dec) == 0 &&
                       VorbisSetupProbe::deferred(session.dec) == 0 && !VorbisSetupProbe::continued(session.dec) && !VorbisSetupProbe::bit_live(session.dec) &&
                       VorbisSetupProbe::first(session.dec) && VorbisSetupProbe::segments(session.dec) == 1;
    expect(clean, "bos_drops_previous_packet", snap(session.dec, parsed));
    const Step body = bos.pull(session.dec);
    Cursor comment{wrap_packet(0x00, comment_payload())};
    comment.pull(session.dec);
    comment.pull(session.dec);
    Cursor setup{wrap_packet(0x00, valid_setup_payload())};
    setup.pull(session.dec);
    const Step setup_body = setup.pull(session.dec);
    const bool no_carry = setup_body.ret >= 0 && VorbisSetupProbe::page(session.dec) == 4 && VorbisSetupProbe::deferred(session.dec) == 0 &&
                          VorbisSetupProbe::dsp(session.dec);
    expect(no_carry, "bos_no_carry_into_audio", snap(session.dec, setup_body));
    Cursor audio{audio_pair()};
    audio.pull(session.dec);
    const Step played = audio.pull(session.dec);
    const bool restored = body.ret >= 0 && setup_body.ret >= 0 && VorbisSetupProbe::page(session.dec) == 4 && VorbisSetupProbe::dsp(session.dec) &&
                          session.dec.getChannels() == 1 && session.dec.getSampleRate() == 44100 && played.ret == 0 && played.used == 1;
    expect(restored, "bos_then_new_audio", snap(session.dec, played));
}

static void test_bos_after_error() {
    Session session;
    if (!reach_audio(session.dec)) {
        expect(false, "bos_err_reach", "chain failed");
        return;
    }
    Cursor saved{filled_page(0x00, lace_packet(100), 0x11)};
    saved.pull(session.dec);
    saved.pull(session.dec);
    auto mem = missing_sync_window();
    const Step bad = decode_at(session.dec, mem.data() + 1, 8192, saved.pcm);
    expect(bad.ret == VorbisDecoder::VORBIS_ERR && VorbisSetupProbe::deferred(session.dec) == 0, "bos_err_cleared", snap(session.dec, bad));
    const bool chain = feed_chain(session.dec, 0x02);
    Cursor audio{audio_pair()};
    audio.pull(session.dec);
    const Step played = audio.pull(session.dec);
    const bool ok = chain && played.ret == 0 && played.used == 1 && VorbisSetupProbe::dsp(session.dec) && VorbisSetupProbe::deferred(session.dec) == 0;
    expect(ok, "bos_after_error_audio", snap(session.dec, played));
}

static void test_setup_then_chain() {
    Session session;
    Cursor ident{wrap_packet(0x02, ident_payload())};
    ident.pull(session.dec);
    ident.pull(session.dec);
    Cursor comment{wrap_packet(0x00, comment_payload())};
    comment.pull(session.dec);
    comment.pull(session.dec);
    std::vector<uint8_t> bad = wrap_packet(0x00, std::vector<uint8_t>{5, 'v', 'o', 'r', 'b', 'i', 's'});
    auto tail = empty_page(0x00);
    bad.insert(bad.end(), tail.begin(), tail.end());
    Cursor setup{std::move(bad)};
    setup.pull(session.dec);
    const Step rejected = setup.pull(session.dec);
    const bool held = rejected.ret < 0 && VorbisSetupProbe::page(session.dec) == 0 && !VorbisSetupProbe::dsp(session.dec) &&
                      VorbisSetupProbe::segments(session.dec) == 0 && !VorbisSetupProbe::continued(session.dec);
    expect(held, "setup_error_holds", snap(session.dec, rejected));
    const bool chain = feed_chain(session.dec, 0x02);
    Cursor audio{audio_pair()};
    audio.pull(session.dec);
    const Step played = audio.pull(session.dec);
    const bool ok = held && chain && played.ret == 0 && VorbisSetupProbe::dsp(session.dec);
    expect(ok, "setup_error_then_chain", snap(session.dec, played));
}

static void test_oversize_then_next() {
    Session session;
    if (!reach_audio(session.dec)) {
        expect(false, "over_reach", "chain failed");
        return;
    }
    Cursor over{filled_page(0x00, lace_packet(4097), 0x11)};
    over.pull(session.dec);
    const Step rejected = over.pull(session.dec);
    const bool cleared = rejected.ret == VorbisDecoder::VORBIS_ERR && VorbisSetupProbe::deferred(session.dec) == 0 &&
                         VorbisSetupProbe::segments(session.dec) == 0 && !VorbisSetupProbe::continued(session.dec);
    expect(cleared, "oversize_clears", snap(session.dec, rejected));
    Cursor next{filled_page(0x00, lace_packet(100), 0x11)};
    const Step parsed = next.pull(session.dec);
    const Step saved = next.pull(session.dec);
    const bool ok = cleared && parsed.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && saved.ret == 0 && VorbisSetupProbe::deferred(session.dec) == 100 &&
                    VorbisSetupProbe::dsp(session.dec);
    expect(ok, "oversize_then_defer", snap(session.dec, saved));
}

static void test_repeated_missing() {
    Session session;
    auto mem = missing_sync_window();
    int32_t pcm[8192]{};
    int stuck = 0;
    Step last{};
    for (int i = 0; i < 4; ++i) {
        last = decode_at(session.dec, mem.data() + 1, 8192, pcm);
        if (last.ret < 0 && last.used == 0 && VorbisSetupProbe::segments(session.dec) == 0) { ++stuck; }
    }
    const bool no_spin_success = stuck == 4 && VorbisSetupProbe::deferred(session.dec) == 0;
    expect(no_spin_success, "repeated_missing_no_progress_success", snap(session.dec, last));

    // decodeError() returns 1 when the decoder consumed nothing. Mirror that
    // advance, then the same window still has no sync and must stay an error.
    const Step skipped = decode_at(session.dec, mem.data() + 2, 8191, pcm);
    expect(skipped.ret == VorbisDecoder::VORBIS_ERR && skipped.used == 0, "after_one_byte_still_error", snap(session.dec, skipped));

    Cursor page{wrap_packet(0x02, ident_payload())};
    const Step parsed = page.pull(session.dec);
    const Step body = page.pull(session.dec);
    const bool recovered = parsed.ret == VorbisDecoder::VORBIS_PARSE_OGG_DONE && body.ret >= 0 && session.dec.getChannels() == 1;
    expect(recovered, "real_page_after_repeated_miss", snap(session.dec, body));
}

int main() {
    std::printf("vorbis_resync\n");
    std::fflush(stdout);
    test_plain_and_audio();
    test_continuation();
    test_continuation_capacity();
    test_noncontinued();
    test_missing_oggs();
    test_stale_after_defer();
    test_continued_same_offset();
    test_bos_after_audio();
    test_bos_after_error();
    test_setup_then_chain();
    test_oversize_then_next();
    test_repeated_missing();
    std::printf("%s  fails=%d\n", g_fails == 0 ? "vorbis_resync: PASS" : "vorbis_resync: FAIL", g_fails);
    return g_fails == 0 ? 0 : 1;
}
