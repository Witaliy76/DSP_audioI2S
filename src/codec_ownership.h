#pragma once

// AF-2.6B / E-HO2. Decisions for HTTP Content-Type vs the live decoder.
// Audio.cpp applies these results. This header does not touch FreeRTOS or sockets.

#include <cstdint>

namespace audiolib {

// How a newly parsed Content-Type codec relates to the active decoder.
enum class CodecApply : uint8_t {
    KeepActive = 0, // leave m_codec alone (same codec, or non-audio CT)
    ApplyNow = 1,   // no live decoder: write active codec immediately
    Stage = 2,      // live decoder, different codec: hold until owned release
};

// What the HTTP body of this response is, independent of any prior m_codec.
enum class HttpBodyKind : uint8_t {
    Audio = 0,
    Playlist = 1,
    Reject = 2, // text/html, bare text/plain, etc. — do not treat as media
};

// CODEC_* values match Audio.h: NONE=0 WAV=1 MP3=2 AAC=3 M4A=4 FLAC=5 OPUS=6 VORBIS=7 OGG=8.
inline bool oggContainerCompatible(uint8_t active_codec, bool active_ogg_flag) {
    if (active_codec == 8) return true;                      // CODEC_OGG
    if (active_codec == 6 || active_codec == 7) return true; // OPUS / VORBIS (always Ogg)
    if (active_codec == 5 && active_ogg_flag) return true;   // Ogg-FLAC only, not native FLAC
    return false;
}

inline CodecApply decideCodecApply(bool decoder_alive, uint8_t active_codec, uint8_t desired_codec,
                                   bool active_ogg_flag = false) {
    if (desired_codec == 0) return CodecApply::KeepActive; // CODEC_NONE: playlist / reject
    if (!decoder_alive) return CodecApply::ApplyNow;
    if (desired_codec == active_codec) return CodecApply::KeepActive;
    // Repeated application/ogg must not tear down an already resolved Ogg subtype.
    if (desired_codec == 8 && oggContainerCompatible(active_codec, active_ogg_flag)) return CodecApply::KeepActive;
    return CodecApply::Stage;
}

// After Content-Type: which dataMode branch lastToDo should take.
inline HttpBodyKind classifyHttpBody(bool ct_is_playlist, bool ct_is_reject, uint8_t active_codec, uint8_t staged_codec,
                                     uint8_t playlist_format) {
    if (ct_is_reject) return HttpBodyKind::Reject;
    // Playlist MIME wins even when a live audio codec remains from the prior body.
    if (ct_is_playlist && playlist_format != 0) return HttpBodyKind::Playlist;
    if (active_codec != 0 || staged_codec != 0) return HttpBodyKind::Audio;
    if (playlist_format != 0) return HttpBodyKind::Playlist;
    return HttpBodyKind::Reject;
}

// Production lastToDo routing after classifyHttpBody (+ optional owned release).
enum class HttpHeaderRoute : uint8_t {
    AudioData = 0,
    PlaylistInit = 1,
    AbortStop = 2, // codec change needed but release failed — do not feed the body
    Reject = 3,
};

inline HttpHeaderRoute routeHttpResponseBody(HttpBodyKind body, bool codec_change_requires_release, bool release_ok) {
    if (body == HttpBodyKind::Reject) return HttpHeaderRoute::Reject;
    if (body == HttpBodyKind::Playlist) return HttpHeaderRoute::PlaylistInit;
    if (codec_change_requires_release && !release_ok) return HttpHeaderRoute::AbortStop;
    return HttpHeaderRoute::AudioData;
}

// Range/seek responses continue the current media. Content-Type must not retarget
// codec ownership or turn the session into a playlist.
inline void freezeCodecOwnershipForRange(uint8_t codec_before, uint8_t playlist_before, bool ogg_before, uint8_t* codec,
                                         uint8_t* staged, uint8_t* playlist, bool* ogg, bool* staged_ogg,
                                         bool* ct_playlist, bool* ct_reject) {
    *codec = codec_before;
    *playlist = playlist_before;
    *ogg = ogg_before;
    *staged = 0;
    *staged_ogg = false;
    *ct_playlist = false;
    *ct_reject = false;
}

// initializeDecoder may return success with a live decoder only when that
// object still matches the required active codec (no pending different stage).
inline bool initializeDecoderAllowsLive(bool decoder_alive, uint8_t active_codec, uint8_t staged_codec) {
    if (!decoder_alive) return true;
    if (staged_codec != 0 && staged_codec != active_codec) return false;
    return true;
}

// True when lastToDo must destroy the live decoder before activating a new codec.
inline bool codecChangeRequiresRelease(bool decoder_alive, uint8_t active_codec, uint8_t staged_codec) {
    return decoder_alive && staged_codec != 0 && staged_codec != active_codec;
}

// Promote staged → active. Returns the new active codec. staged_out becomes 0.
inline uint8_t applyStagedCodec(uint8_t staged_codec, uint8_t* staged_out) {
    *staged_out = 0;
    return staged_codec;
}

// Pending Stop/Reconnect from AF-2.6A must not be demoted by a codec stage clear.
// Clearing the stage alone never touches DecoderCtrl.
inline bool codecStageIndependentOfDecoderCtrl() { return true; }

} // namespace audiolib
