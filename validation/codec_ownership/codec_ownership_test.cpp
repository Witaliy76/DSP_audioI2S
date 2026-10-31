// AF-2.6B / E-HO2. Drives production decisions in src/codec_ownership.h.
// A–G cover the codec contract. R1 adds lastToDo routing used by Audio.cpp.

#include "../../src/codec_ownership.h"
#include "../../src/decoder_lifecycle.h"

#include <atomic>
#include <cstdio>

namespace {

int g_failed = 0;

void check(bool cond, const char* expr, int line) {
    if (cond) return;
    std::printf("FAIL %s:%d: %s\n", "codec_ownership_test.cpp", line, expr);
    ++g_failed;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

// Mirror Audio::CODEC_* / FORMAT_* values.
constexpr uint8_t CODEC_NONE = 0;
constexpr uint8_t CODEC_MP3 = 2;
constexpr uint8_t CODEC_AAC = 3;
constexpr uint8_t CODEC_FLAC = 5;
constexpr uint8_t CODEC_OPUS = 6;
constexpr uint8_t CODEC_VORBIS = 7;
constexpr uint8_t CODEC_OGG = 8;
constexpr uint8_t FORMAT_NONE = 0;
constexpr uint8_t FORMAT_M3U = 1;
constexpr uint8_t FORMAT_PLS = 2;
constexpr uint8_t FORMAT_M3U8 = 4;

struct CodecWorld {
    bool    decoder_alive = false;
    uint8_t active = CODEC_NONE;
    uint8_t staged = CODEC_NONE;
    uint8_t playlist = FORMAT_NONE;
    bool    ogg = false;
    bool    ct_playlist = false;
    bool    ct_reject = false;
    int     releases = 0;
    int     applies = 0;
    int     inits_ok = 0;
    int     inits_fail = 0;

    void on_content_type(uint8_t desired, bool is_playlist, bool is_reject, uint8_t pls = FORMAT_M3U8) {
        ct_playlist = is_playlist;
        ct_reject = is_reject;
        if (is_playlist && desired == CODEC_NONE) playlist = pls;

        using audiolib::CodecApply;
        switch (audiolib::decideCodecApply(decoder_alive, active, desired, ogg)) {
            case CodecApply::KeepActive:
                staged = CODEC_NONE;
                break;
            case CodecApply::ApplyNow:
                active = desired;
                staged = CODEC_NONE;
                if (desired != CODEC_NONE) ogg = (desired == CODEC_OGG || desired == CODEC_OPUS || desired == CODEC_VORBIS);
                break;
            case CodecApply::Stage:
                staged = desired;
                break;
        }
    }

    audiolib::HttpBodyKind body() const {
        return audiolib::classifyHttpBody(ct_playlist, ct_reject, active, staged, playlist);
    }

    audiolib::HttpHeaderRoute route(bool release_ok) const {
        const bool need = audiolib::codecChangeRequiresRelease(decoder_alive, active, staged);
        return audiolib::routeHttpResponseBody(body(), need, release_ok);
    }

    bool try_release_for_change() {
        if (!audiolib::codecChangeRequiresRelease(decoder_alive, active, staged)) {
            if (!decoder_alive && staged != CODEC_NONE) {
                active = audiolib::applyStagedCodec(staged, &staged);
                ++applies;
            }
            return true;
        }
        decoder_alive = false;
        ++releases;
        active = audiolib::applyStagedCodec(staged, &staged);
        ++applies;
        return true;
    }

    bool initialize() {
        if (!decoder_alive && staged != CODEC_NONE) {
            active = audiolib::applyStagedCodec(staged, &staged);
            ++applies;
        }
        if (!audiolib::initializeDecoderAllowsLive(decoder_alive, active, staged)) {
            ++inits_fail;
            return false;
        }
        if (!decoder_alive) decoder_alive = true;
        ++inits_ok;
        return true;
    }

    void clear_stage() {
        staged = CODEC_NONE;
        ct_playlist = false;
        ct_reject = false;
    }
};

void scenario_A_live_mp3_gets_aac_content_type() {
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_MP3;

    w.on_content_type(CODEC_AAC, false, false);
    CHECK(w.active == CODEC_MP3);
    CHECK(w.staged == CODEC_AAC);
    CHECK(!audiolib::initializeDecoderAllowsLive(true, w.active, w.staged));
    CHECK(w.body() == audiolib::HttpBodyKind::Audio);
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::AudioData);
}

void scenario_B_mp3_to_flac_after_owned_release() {
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_MP3;

    w.on_content_type(CODEC_FLAC, false, false);
    CHECK(w.staged == CODEC_FLAC);
    CHECK(w.try_release_for_change());
    CHECK(!w.decoder_alive);
    CHECK(w.active == CODEC_FLAC);
    CHECK(w.staged == CODEC_NONE);
    CHECK(w.initialize());
    CHECK(w.decoder_alive);
    CHECK(w.active == CODEC_FLAC);
    CHECK(w.releases == 1);
}

void scenario_C_same_codec_does_not_stage() {
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_MP3;

    w.on_content_type(CODEC_MP3, false, false);
    CHECK(w.active == CODEC_MP3);
    CHECK(w.staged == CODEC_NONE);
    CHECK(audiolib::initializeDecoderAllowsLive(true, w.active, w.staged));
    CHECK(w.initialize());
    CHECK(w.releases == 0);
    CHECK(w.decoder_alive);
}

void scenario_D_playlist_does_not_replace_active_decoder() {
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_AAC;

    w.on_content_type(CODEC_NONE, true, false, FORMAT_M3U8);
    CHECK(w.active == CODEC_AAC);
    CHECK(w.staged == CODEC_NONE);
    CHECK(w.body() == audiolib::HttpBodyKind::Playlist);
    // Production lastToDo must choose PLAYLISTINIT, never AUDIO_DATA.
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::PlaylistInit);
    CHECK(w.initialize());
    CHECK(w.active == CODEC_AAC);
    CHECK(w.releases == 0);
}

void scenario_E_ogg_response_applies_ogg_for_subtype() {
    CodecWorld w;
    w.on_content_type(CODEC_OGG, false, false);
    CHECK(w.active == CODEC_OGG);
    CHECK(w.staged == CODEC_NONE);
    CHECK(w.body() == audiolib::HttpBodyKind::Audio);
    CHECK(w.initialize());
    CHECK(w.active == CODEC_OGG);
}

void scenario_F_error_clears_stage() {
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_MP3;
    w.on_content_type(CODEC_AAC, false, false);
    CHECK(w.staged == CODEC_AAC);
    w.clear_stage();
    CHECK(w.staged == CODEC_NONE);
    CHECK(audiolib::initializeDecoderAllowsLive(true, w.active, w.staged));
}

void scenario_G_stage_independent_of_decoder_ctrl() {
    CHECK(audiolib::codecStageIndependentOfDecoderCtrl());

    std::atomic<uint8_t> ctrl{0};
    audiolib::requestDecoderCtrl(ctrl, audiolib::DecoderCtrl::Reconnect);

    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_MP3;
    w.on_content_type(CODEC_AAC, false, false);
    w.clear_stage();

    CHECK(ctrl.load(std::memory_order_acquire) == static_cast<uint8_t>(audiolib::DecoderCtrl::Reconnect));

    w.on_content_type(CODEC_FLAC, false, false);
    audiolib::requestDecoderCtrl(ctrl, audiolib::DecoderCtrl::Stop);
    CHECK(ctrl.load(std::memory_order_acquire) == static_cast<uint8_t>(audiolib::DecoderCtrl::Stop));
    CHECK(w.staged == CODEC_FLAC);
}

void scenario_reject_text_plain() {
    CodecWorld w;
    w.on_content_type(CODEC_NONE, false, true);
    CHECK(w.body() == audiolib::HttpBodyKind::Reject);
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::Reject);
}

// --- R1 integration routing (production routeHttpResponseBody / classifyHttpBody) ---

void r1_playlist_live_decoder_not_audio_data() {
    // Live MP3 + M3U8 MIME → PLAYLISTINIT. Decoder must not see playlist text as audio.
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_MP3;
    w.on_content_type(CODEC_NONE, true, false, FORMAT_M3U8);
    CHECK(w.body() == audiolib::HttpBodyKind::Playlist);
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::PlaylistInit);

    w.on_content_type(CODEC_NONE, true, false, FORMAT_M3U);
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::PlaylistInit);

    w.on_content_type(CODEC_NONE, true, false, FORMAT_PLS);
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::PlaylistInit);
}

void r1_audio_codec_change_and_failed_release() {
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_MP3;
    w.on_content_type(CODEC_AAC, false, false);
    CHECK(w.body() == audiolib::HttpBodyKind::Audio);
    CHECK(audiolib::codecChangeRequiresRelease(true, w.active, w.staged));
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::AudioData);
    // Mutex timeout: production goes to exit/stopSong — body must not become AUDIO_DATA.
    CHECK(w.route(false) == audiolib::HttpHeaderRoute::AbortStop);
    CHECK(w.active == CODEC_MP3); // still protected
    CHECK(w.staged == CODEC_AAC);
}

void r1_redirect_target_playlist_with_live_decoder() {
    // After Location → httpPrint, next CT is playlist while old decoder may still live.
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_AAC;
    w.on_content_type(CODEC_NONE, true, false, FORMAT_M3U8);
    CHECK(w.active == CODEC_AAC);
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::PlaylistInit);
}

void r1_ogg_ct_keeps_resolved_subtype() {
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_VORBIS;
    w.ogg = true;
    w.on_content_type(CODEC_OGG, false, false);
    CHECK(w.active == CODEC_VORBIS);
    CHECK(w.staged == CODEC_NONE);
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::AudioData);

    w.active = CODEC_OPUS;
    w.on_content_type(CODEC_OGG, false, false);
    CHECK(w.staged == CODEC_NONE);

    // Native FLAC (ogg flag false) + application/ogg is a real change → stage.
    w.active = CODEC_FLAC;
    w.ogg = false;
    w.on_content_type(CODEC_OGG, false, false);
    CHECK(w.staged == CODEC_OGG);
}

void r1_range_freezes_ownership() {
    uint8_t codec = CODEC_MP3;
    uint8_t staged = CODEC_AAC;
    uint8_t playlist = FORMAT_M3U8;
    bool    ogg = false;
    bool    staged_ogg = true;
    bool    ct_pl = true;
    bool    ct_rj = true;
    audiolib::freezeCodecOwnershipForRange(CODEC_MP3, FORMAT_NONE, false, &codec, &staged, &playlist, &ogg, &staged_ogg,
                                           &ct_pl, &ct_rj);
    CHECK(codec == CODEC_MP3);
    CHECK(staged == CODEC_NONE);
    CHECK(playlist == FORMAT_NONE);
    CHECK(!ogg);
    CHECK(!staged_ogg);
    CHECK(!ct_pl);
    CHECK(!ct_rj);
}

void r1_playlist_then_media_keeps_route() {
    CodecWorld w;
    w.decoder_alive = true;
    w.active = CODEC_AAC;
    // Playlist refresh
    w.on_content_type(CODEC_NONE, true, false, FORMAT_M3U8);
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::PlaylistInit);
    // Next media segment same codec
    w.ct_playlist = false;
    w.on_content_type(CODEC_AAC, false, false);
    CHECK(w.active == CODEC_AAC);
    CHECK(w.staged == CODEC_NONE);
    CHECK(w.route(true) == audiolib::HttpHeaderRoute::AudioData);
    CHECK(w.releases == 0);
}

} // namespace

int main() {
    scenario_A_live_mp3_gets_aac_content_type();
    scenario_B_mp3_to_flac_after_owned_release();
    scenario_C_same_codec_does_not_stage();
    scenario_D_playlist_does_not_replace_active_decoder();
    scenario_E_ogg_response_applies_ogg_for_subtype();
    scenario_F_error_clears_stage();
    scenario_G_stage_independent_of_decoder_ctrl();
    scenario_reject_text_plain();

    r1_playlist_live_decoder_not_audio_data();
    r1_audio_codec_change_and_failed_release();
    r1_redirect_target_playlist_with_live_decoder();
    r1_ogg_ct_keeps_resolved_subtype();
    r1_range_freezes_ownership();
    r1_playlist_then_media_keeps_route();

    if (g_failed) {
        std::printf("codec_ownership: FAIL (%d)\n", g_failed);
        return 1;
    }
    std::printf("codec_ownership: PASS\n");
    return 0;
}
