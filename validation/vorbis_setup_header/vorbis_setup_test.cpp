// AF-2.3. Compiles production src/vorbis_decoder. The friend probe only reads state.
#include "vorbis_decoder.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct VorbisSetupProbe {
    static uint8_t page(const VorbisDecoder& d) { return d.m_pageNr; }
    static uint8_t modes(const VorbisDecoder& d) { return d.m_nrOfModes; }
    static uint8_t books(const VorbisDecoder& d) { return d.m_nrOfCodebooks; }
    static uint8_t floors(const VorbisDecoder& d) { return d.m_nrOfFloors; }
    static bool    dsp(const VorbisDecoder& d) { return d.m_dsp_state.valid(); }
    static bool    mode_array(const VorbisDecoder& d) { return d.m_mode_param.valid(); }
};

static int g_fails = 0;

struct Session {
    Audio audio;
    VorbisDecoder dec;
    explicit Session() : dec(audio) { dec.init(9216); }
};

static bool named(const char* name) {
    for (const HostAlloc& a : host_allocs) {
        if (a.name != nullptr && std::strcmp(a.name, name) == 0) { return true; }
    }
    return false;
}

static void expect(bool cond, const char* name, const std::string& detail) {
    if (cond) {
        std::printf("  PASS  %s\n", name);
        return;
    }
    std::printf("  FAIL  %s  %s\n", name, detail.c_str());
    ++g_fails;
}

struct Bits {
    std::vector<uint8_t> bytes;
    int                  filled = 0;
    void put(uint32_t value, int n) {
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

static void put_codebook(Bits& w) {
    // One used entry and six unused sparse entries. The extra flag bits make
    // the prefix through the time backend a whole number of bytes, so the
    // next count starts past the buffer instead of on zero padding.
    w.put(0x564342, 24);
    w.put(1, 16);
    w.put(7, 24);
    w.put(0, 1);
    w.put(1, 1);
    w.put(1, 1);
    w.put(0, 5);
    for (int i = 0; i < 6; ++i) { w.put(0, 1); }
    w.put(0, 4);
}

static void put_time(Bits& w) {
    w.put(0, 6);
    w.put(0, 16);
}

static void put_floor(Bits& w) {
    w.put(0, 16);
    w.put(1, 8);
    w.put(1, 16);
    w.put(1, 16);
    w.put(0, 6);
    w.put(0, 8);
    w.put(0, 4);
    w.put(0, 8);
}

static void put_residue(Bits& w) {
    w.put(0, 16);
    w.put(0, 24);
    w.put(0, 24);
    w.put(0, 24);
    w.put(0, 6);
    w.put(0, 8);
    w.put(0, 3);
    w.put(0, 1);
}

static void put_mapping(Bits& w) {
    w.put(0, 16);
    w.put(1, 1);
    w.put(0, 4);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 8);
    w.put(0, 8);
    w.put(0, 8);
}

static void put_mode(Bits& w, uint32_t mapping) {
    w.put(0, 1);
    w.put(0, 16);
    w.put(0, 16);
    w.put(mapping, 8);
}

enum class SetupKind {
    EofBooks,
    EofFloors,
    EofResidues,
    EofMaps,
    EofModes,
    BadBook,
    BadMap,
    BadModeMap,
    NoFraming,
    FloorOrderZero,
    CodebookCount255,
    Good
};

static std::vector<uint8_t> setup_bits(SetupKind kind) {
    Bits w;
    if (kind == SetupKind::EofBooks) { return w.bytes; }
    if (kind == SetupKind::CodebookCount255) {
        w.put(255, 8);
        return w.bytes;
    }
    if (kind == SetupKind::BadBook) {
        w.put(0, 8);
        w.put(0, 24);
        return w.bytes;
    }
    w.put(0, 8);
    put_codebook(w);
    put_time(w);
    if (kind == SetupKind::EofFloors) { return w.bytes; }
    w.put(0, 6);
    if (kind == SetupKind::FloorOrderZero) {
        w.put(0, 16);
        w.put(0, 8);
        return w.bytes;
    }
    put_floor(w);
    if (kind == SetupKind::EofResidues) { return w.bytes; }
    w.put(0, 6);
    put_residue(w);
    if (kind == SetupKind::EofMaps) { return w.bytes; }
    w.put(0, 6);
    if (kind == SetupKind::BadMap) {
        w.put(1, 16);
        return w.bytes;
    }
    put_mapping(w);
    if (kind == SetupKind::EofModes) { return w.bytes; }
    w.put(0, 6);
    put_mode(w, kind == SetupKind::BadModeMap ? 1u : 0u);
    if (kind == SetupKind::NoFraming) {
        w.put(0, 1);
        return w.bytes;
    }
    w.put(1, 1);
    return w.bytes;
}

static void append_page(std::vector<uint8_t>& buf, const std::vector<uint8_t>& payload, uint8_t headerType) {
    const size_t at = buf.size();
    buf.resize(at + 27 + 1 + payload.size());
    std::memcpy(buf.data() + at, "OggS", 4);
    buf[at + 5] = headerType;
    buf[at + 26] = 1;
    buf[at + 27] = static_cast<uint8_t>(payload.size());
    std::memcpy(buf.data() + at + 28, payload.data(), payload.size());
}

static void append_empty_page(std::vector<uint8_t>& buf) {
    const size_t at = buf.size();
    buf.resize(at + 27);
    std::memcpy(buf.data() + at, "OggS", 4);
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

static std::vector<uint8_t> setup_payload(SetupKind kind) {
    std::vector<uint8_t> bits = setup_bits(kind);
    std::vector<uint8_t> p(7 + bits.size(), 0);
    p[0] = 5;
    std::memcpy(p.data() + 1, "vorbis", 6);
    if (!bits.empty()) { std::memcpy(p.data() + 7, bits.data(), bits.size()); }
    return p;
}

struct Step {
    int32_t ret = 0;
    int32_t used = 0;
};

struct Drive {
    std::vector<Step> steps;
    int32_t ret() const { return steps.empty() ? 0 : steps.back().ret; }
    int32_t used() const { return steps.empty() ? 0 : steps.back().used; }
};

static Drive drive(VorbisDecoder& dec, std::vector<uint8_t> page, int calls) {
    uint8_t* p = page.data();
    int32_t left = static_cast<int32_t>(page.size());
    int32_t pcm[8192];
    Drive d;
    for (int i = 0; i < calls && left > 0; ++i) {
        const int32_t before = left;
        const int32_t ret = dec.decode(p, &left, pcm);
        const int32_t used = before - left;
        d.steps.push_back(Step{ret, used});
        if (used < 0) { break; }
        p += used;
    }
    return d;
}

static bool feed_headers(VorbisDecoder& dec) {
    std::vector<uint8_t> ident;
    append_page(ident, ident_payload(), 0x02);
    const Drive id = drive(dec, ident, 2);
    std::vector<uint8_t> comment;
    append_page(comment, comment_payload(), 0x00);
    const Drive co = drive(dec, comment, 2);
    return id.steps.size() == 2 && co.steps.size() == 2 && id.ret() >= 0 && co.ret() >= 0 && dec.getChannels() == 1 && dec.getSampleRate() == 44100;
}

static Drive feed_setup(VorbisDecoder& dec, SetupKind kind) {
    std::vector<uint8_t> page;
    append_page(page, setup_payload(kind), 0x00);
    append_empty_page(page);
    return drive(dec, page, 2);
}

static std::vector<uint8_t> audio_page() {
    std::vector<uint8_t> payload{0x00, 0x00};
    const size_t at = 0;
    std::vector<uint8_t> buf(27 + 2 + payload.size(), 0);
    std::memcpy(buf.data() + at, "OggS", 4);
    buf[26] = 2;
    buf[27] = 1;
    buf[28] = 1;
    std::memcpy(buf.data() + 29, payload.data(), payload.size());
    return buf;
}

static std::string state_of(const VorbisDecoder& dec, int32_t ret, int32_t used) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "ret=%d used=%d page=%u dsp=%d modes=%u mode_array=%d books=%u floors=%u", ret, used,
                  VorbisSetupProbe::page(dec), VorbisSetupProbe::dsp(dec) ? 1 : 0, VorbisSetupProbe::modes(dec),
                  VorbisSetupProbe::mode_array(dec) ? 1 : 0, VorbisSetupProbe::books(dec), VorbisSetupProbe::floors(dec));
    return buf;
}

static std::string alloc_dump() {
    std::string s;
    for (const HostAlloc& a : host_allocs) {
        if (!s.empty()) { s += ","; }
        s += a.name != nullptr ? a.name : "-";
    }
    return s;
}

static bool banned_hit(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (named(name)) { return true; }
    }
    return false;
}

static void expect_rejected(const char* name, SetupKind kind, std::initializer_list<const char*> banned) {
    Session session;
    VorbisDecoder& dec = session.dec;
    const bool headers = feed_headers(dec);
    host_allocs.clear();
    const Drive s = feed_setup(dec, kind);
    const bool later = banned_hit(banned);
    const std::string st = state_of(dec, s.ret(), s.used()) + " headers=" + std::to_string(headers) + " allocs=" + alloc_dump();
    const bool count_rejected = kind == SetupKind::EofBooks || kind == SetupKind::BadBook || kind == SetupKind::CodebookCount255;
    const bool books_ok = !count_rejected || VorbisSetupProbe::books(dec) == 0;
    const bool ok = headers && s.steps.size() == 2 && s.steps[0].ret >= 0 && s.ret() == -133 && s.used() > 0 && VorbisSetupProbe::page(dec) != 4 &&
                    !VorbisSetupProbe::dsp(dec) && !later && books_ok;
    expect(ok, name, st);
    std::printf("        %s\n", st.c_str());
    std::fflush(stdout);
}

int main() {
    std::printf("vorbis_setup\n");
    std::fflush(stdout);
    const char* after_books[] = {"m_codebooks", "m_floor_param", "m_residue_param", "m_map_param", "m_mode_param"};
    const char* from_floors[] = {"m_floor_param", "m_residue_param", "m_map_param", "m_mode_param"};
    const char* from_residues[] = {"m_residue_param", "m_map_param", "m_mode_param"};
    const char* from_maps[] = {"m_map_param", "m_mode_param"};
    const char* from_modes[] = {"m_mode_param"};
    expect_rejected("eof_codebooks", SetupKind::EofBooks, {after_books[0], after_books[1], after_books[2], after_books[3], after_books[4]});
    expect_rejected("codebook_count_255", SetupKind::CodebookCount255, {after_books[0], after_books[1], after_books[2], after_books[3], after_books[4]});
    expect_rejected("eof_floors", SetupKind::EofFloors, {from_floors[0], from_floors[1], from_floors[2], from_floors[3]});
    expect_rejected("eof_residues", SetupKind::EofResidues, {from_residues[0], from_residues[1], from_residues[2]});
    expect_rejected("eof_mappings", SetupKind::EofMaps, {from_maps[0], from_maps[1]});
    expect_rejected("eof_modes", SetupKind::EofModes, {from_modes[0]});
    expect_rejected("bad_codebook", SetupKind::BadBook, {from_floors[0], from_floors[1], from_floors[2], from_floors[3]});
    expect_rejected("bad_mapping", SetupKind::BadMap, {from_modes[0]});
    expect_rejected("bad_mode_mapping", SetupKind::BadModeMap, {});
    expect_rejected("missing_framing", SetupKind::NoFraming, {});

    {
        Session session;
        VorbisDecoder& dec = session.dec;
        const bool headers = feed_headers(dec);
        const Drive bad = feed_setup(dec, SetupKind::EofBooks);
        std::vector<uint8_t> stray_page;
        // Longer than the 10-byte "vorbis" probe so the search cannot read past this page.
        append_page(stray_page, std::vector<uint8_t>(16, 0), 0x00);
        const Drive stray = drive(dec, stray_page, 2);
        // The failed setup segment was consumed. This non-header segment stays on page 0
        // and is not synthesized. One segment, so the table is empty afterwards.
        const bool held = headers && bad.ret() < 0 && bad.used() > 0 && VorbisSetupProbe::page(dec) == 0 && !VorbisSetupProbe::dsp(dec) &&
                          stray.steps.size() == 2 && stray.ret() == 100 && stray.used() == 0;
        expect(held, "reject_then_hold", state_of(dec, stray.ret(), stray.used()));
        std::fflush(stdout);

        const bool again = feed_headers(dec);
        const Drive good = feed_setup(dec, SetupKind::Good);
        const bool recovered = again && good.ret() >= 0 && VorbisSetupProbe::page(dec) == 4 && VorbisSetupProbe::dsp(dec) &&
                               VorbisSetupProbe::modes(dec) == 1 && VorbisSetupProbe::mode_array(dec);
        expect(recovered, "retry_full_chain", state_of(dec, good.ret(), good.used()));
        const Drive decoded = drive(dec, audio_page(), 2);
        const bool audio_ok = decoded.steps.size() == 2 && decoded.ret() == 0 && decoded.used() == 1 && VorbisSetupProbe::page(dec) == 4 &&
                              VorbisSetupProbe::dsp(dec);
        expect(audio_ok, "first_audio", state_of(dec, decoded.ret(), decoded.used()));
        std::fflush(stdout);
    }

    {
        Session session;
        VorbisDecoder& dec = session.dec;
        feed_headers(dec);
        const Drive good = feed_setup(dec, SetupKind::Good);
        expect(good.ret() >= 0 && VorbisSetupProbe::page(dec) == 4 && VorbisSetupProbe::modes(dec) == 1 && VorbisSetupProbe::dsp(dec),
               "good_setup", state_of(dec, good.ret(), good.used()));
        std::vector<uint8_t> ident;
        append_page(ident, ident_payload(), 0x02);
        drive(dec, ident, 2);
        const bool cleared = VorbisSetupProbe::modes(dec) == 0 && !VorbisSetupProbe::mode_array(dec) && !VorbisSetupProbe::dsp(dec);
        expect(cleared, "modes_cleared", state_of(dec, 0, 0));
        std::vector<uint8_t> comment;
        append_page(comment, comment_payload(), 0x00);
        drive(dec, comment, 2);
        const Drive bad = feed_setup(dec, SetupKind::EofBooks);
        const bool safe = bad.ret() < 0 && bad.used() > 0 && VorbisSetupProbe::page(dec) != 4 && !VorbisSetupProbe::dsp(dec) &&
                          VorbisSetupProbe::modes(dec) == 0 && !VorbisSetupProbe::mode_array(dec);
        expect(safe, "good_then_malformed", state_of(dec, bad.ret(), bad.used()));
        std::fflush(stdout);
    }

    {
        Session session;
        VorbisDecoder& dec = session.dec;
        feed_headers(dec);
        const Drive bad = feed_setup(dec, SetupKind::FloorOrderZero);
        expect(bad.ret() < 0 && VorbisSetupProbe::page(dec) != 4 && !VorbisSetupProbe::dsp(dec), "floor_unpack_rejected",
               state_of(dec, bad.ret(), bad.used()));
        std::printf("        floor retry starts\n");
        std::fflush(stdout);
        feed_headers(dec);
        const Drive good = feed_setup(dec, SetupKind::Good);
        const bool recovered = good.ret() >= 0 && VorbisSetupProbe::page(dec) == 4 && VorbisSetupProbe::dsp(dec);
        expect(recovered, "floor_unpack_then_retry", state_of(dec, good.ret(), good.used()));
        std::fflush(stdout);
    }

    {
        // One object: floor failure, then another failed setup, then a valid chain.
        // Each new identification runs clearGlobalConfigurations before parsing.
        Session session;
        VorbisDecoder& dec = session.dec;
        const bool first_headers = feed_headers(dec);
        const Drive floor_bad = feed_setup(dec, SetupKind::FloorOrderZero);
        const bool first_bad = first_headers && floor_bad.ret() == -133 && VorbisSetupProbe::page(dec) == 0 && !VorbisSetupProbe::dsp(dec) &&
                               VorbisSetupProbe::floors(dec) == 1;
        expect(first_bad, "chain_floor_fail", state_of(dec, floor_bad.ret(), floor_bad.used()));

        const bool second_headers = feed_headers(dec);
        const bool cleared = second_headers && VorbisSetupProbe::floors(dec) == 0 && VorbisSetupProbe::modes(dec) == 0 && !VorbisSetupProbe::dsp(dec);
        expect(cleared, "chain_cleared_after_floor", state_of(dec, 0, 0));
        const Drive eof_bad = feed_setup(dec, SetupKind::CodebookCount255);
        const bool second_bad = eof_bad.ret() == -133 && eof_bad.used() > 0 && VorbisSetupProbe::page(dec) == 0 && !VorbisSetupProbe::dsp(dec) &&
                                VorbisSetupProbe::books(dec) == 0 && VorbisSetupProbe::floors(dec) == 0;
        expect(second_bad, "chain_second_fail", state_of(dec, eof_bad.ret(), eof_bad.used()));

        const bool third_headers = feed_headers(dec);
        const Drive good = feed_setup(dec, SetupKind::Good);
        const Drive decoded = drive(dec, audio_page(), 2);
        const bool restored = third_headers && good.ret() >= 0 && VorbisSetupProbe::page(dec) == 4 && VorbisSetupProbe::dsp(dec) &&
                              VorbisSetupProbe::modes(dec) == 1 && decoded.ret() == 0 && decoded.used() == 1;
        expect(restored, "chain_fail_fail_valid", state_of(dec, decoded.ret(), decoded.used()));
        std::fflush(stdout);
    }

    std::printf("%s  fails=%d\n", g_fails == 0 ? "vorbis_setup: PASS" : "vorbis_setup: FAIL", g_fails);
    return g_fails == 0 ? 0 : 1;
}
