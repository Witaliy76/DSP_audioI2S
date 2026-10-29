#pragma once
// Startup prebuffer contract for AF-2.5 / E-FL3.
//
// m_frameSizeFLAC (UINT16_MAX) is the maximum input window a FLAC frame or Ogg
// page is allowed to occupy. It is not the number of bytes required before the
// first decode. A native frame is runnable only when its CRC-16 footer matches,
// proven either because the buffer ends on that frame or because the next
// header this decoder accepts (0xFFF8) follows it. An Ogg page is runnable once
// its lacing table is fully present. Anything else keeps the capacity gate.
// 0xFFF9 is a legal sync in RFC 9639, but decodeFrame() accepts only 0xFFF8,
// so this scanner does not treat 0xFFF9 as a frame.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace audiolib {

constexpr uint32_t kStartupStallMs = 8000;

// Predicates extracted from processWebStream / processWebFile / playAudioData
// before AF-2.5. Audio no longer calls them. The host fixture locks the
// measured baseline against these formulas.
inline bool legacyWebStreamReady(size_t filled, size_t maxFrameSize, bool allDataReceived) {
    return (filled > maxFrameSize) || allDataReceived;
}

inline bool legacyWebFileReady(size_t filled, size_t maxBlock) {
    return filled > (2 * maxBlock);
}

inline bool legacyDecodeAllowed(size_t readSpace, size_t maxBlock, bool lastFrames) {
    if (lastFrames) return true;
    return readSpace >= maxBlock;
}

namespace flac_startup_detail {

inline uint8_t crc8(const uint8_t* p, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc = static_cast<uint8_t>(crc ^ p[i]);
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80)
                crc = static_cast<uint8_t>((crc << 1) ^ 0x07);
            else
                crc = static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

// FLAC frame-number UTF-8. Returns the byte length, or 0 when the sequence
// is invalid or does not fit in `avail`.
inline int utf8Len(const uint8_t* p, size_t avail) {
    if (avail == 0) return 0;
    const uint8_t c = p[0];
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xC0) == 0x80) return 0;
    int     len = 0;
    uint8_t mask = 0;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        mask = 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        mask = 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        mask = 0x07;
    } else if ((c & 0xFC) == 0xF8) {
        len = 5;
        mask = 0x03;
    } else if ((c & 0xFE) == 0xFC) {
        len = 6;
        mask = 0x01;
    } else if (c == 0xFE) {
        len = 7;
        mask = 0x00;
    } else {
        return 0;
    }
    if (static_cast<size_t>(len) > avail) return 0;
    uint64_t value = c & mask;
    for (int i = 1; i < len; ++i) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        value = (value << 6) | (p[i] & 0x3F);
    }
    if (value >= (1ULL << 36)) return 0;
    return len;
}

// FLAC CRC-16, polynomial 0x8005, init 0. Covers the frame except its footer.
inline uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(p[i]) << 8));
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000)
                crc = static_cast<uint16_t>((crc << 1) ^ 0x8005);
            else
                crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

inline bool frameCrc16Matches(const uint8_t* frame, size_t frame_len) {
    if (frame == nullptr || frame_len < 2) return false;
    const uint16_t got = static_cast<uint16_t>((static_cast<uint16_t>(frame[frame_len - 2]) << 8) | frame[frame_len - 1]);
    return crc16(frame, frame_len - 2) == got;
}

// Byte length of a native frame header that decodeFrame will accept (sync
// 0xFFF8 only). 0 when the header is incomplete, reserved, or the CRC-8
// does not match. A 0xFFF9 header is not accepted.
inline size_t frameHeaderLen(const uint8_t* p, size_t avail) {
    if (avail < 6) return 0;
    if (p[0] != 0xFF || p[1] != 0xF8) return 0;
    const uint8_t blocksize = (p[2] >> 4) & 0x0F;
    const uint8_t rate = p[2] & 0x0F;
    const uint8_t channels = (p[3] >> 4) & 0x0F;
    const uint8_t bps = (p[3] >> 1) & 0x07;
    const uint8_t reserved = p[3] & 0x01;
    if (reserved || blocksize == 0 || rate == 0x0F || channels > 10 || bps == 0 || bps == 7) return 0;
    const int utf8 = utf8Len(p + 4, avail - 4);
    if (utf8 <= 0) return 0;
    size_t header = static_cast<size_t>(4 + utf8);
    if (blocksize == 6) header += 1;
    else if (blocksize == 7) header += 2;
    if (rate == 12) header += 1;
    else if (rate == 13 || rate == 14) header += 2;
    header += 1;
    if (header > avail) return 0;
    if (crc8(p, header - 1) != p[header - 1]) return 0;
    return header;
}

inline size_t findFrameHeader(const uint8_t* p, size_t len, size_t from) {
    if (from >= len) return static_cast<size_t>(-1);
    for (size_t i = from; i + 6 <= len; ++i) {
        if (i > 0 && p[i - 1] == 0xFF) continue;
        if (frameHeaderLen(p + i, len - i) != 0) return i;
    }
    return static_cast<size_t>(-1);
}

// Bytes from the start of `p` through one complete native frame. The buffer
// must begin on a header decodeFrame() will accept. A later CRC-8 header is
// the end only when the bytes in front of it carry a matching CRC-16 footer.
// A header embedded in the payload fails that footer check and is skipped.
// A partial header is not an end. Returns 0 when the frame is not proven;
// the caller then keeps the capacity gate.
inline size_t nativeUnitFrom(const uint8_t* p, size_t len) {
    const size_t hlen = frameHeaderLen(p, len);
    if (hlen == 0) return 0;
    size_t from = hlen;
    while (from < len) {
        const size_t next = findFrameHeader(p, len, from);
        if (next == static_cast<size_t>(-1)) break;
        if (next >= hlen + 2 && frameCrc16Matches(p, next)) return next;
        from = next + 1;
    }
    if (len >= hlen + 2 && frameCrc16Matches(p, len)) return len;
    return 0;
}

inline size_t oggPageLen(const uint8_t* p, size_t len) {
    if (len < 27) return 0;
    if (std::memcmp(p, "OggS", 4) != 0) return 0;
    const size_t nseg = p[26];
    if (len < 27 + nseg) return 0;
    size_t payload = 0;
    for (size_t i = 0; i < nseg; ++i) payload += p[27 + i];
    const size_t total = 27 + nseg + payload;
    if (len < total) return 0;
    return total;
}

inline size_t flacMetadataEnd(const uint8_t* p, size_t len) {
    if (len < 8 || std::memcmp(p, "fLaC", 4) != 0) return 0;
    size_t pos = 4;
    for (;;) {
        if (pos + 4 > len) return 0;
        const bool     last = (p[pos] & 0x80) != 0;
        const uint32_t blen = (static_cast<uint32_t>(p[pos + 1]) << 16) | (static_cast<uint32_t>(p[pos + 2]) << 8) | p[pos + 3];
        pos += 4;
        if (blen > len - pos) return 0;
        pos += blen;
        if (last) return pos;
    }
}

} // namespace flac_startup_detail

// Bytes from the start of `data` that contain one complete Ogg page, or one
// complete native FLAC frame (optionally after a fully buffered fLaC metadata
// prefix or a short leading skip). 0 if that unit is not fully present.
inline size_t flacContainedUnit(const uint8_t* data, size_t len) {
    using namespace flac_startup_detail;
    if (data == nullptr || len < 4) return 0;
    if (std::memcmp(data, "OggS", 4) == 0) return oggPageLen(data, len);
    if (std::memcmp(data, "fLaC", 4) == 0) {
        const size_t audio = flacMetadataEnd(data, len);
        if (audio == 0 || audio >= len) return 0;
        const size_t frame = nativeUnitFrom(data + audio, len - audio);
        if (frame == 0) return 0;
        return audio + frame;
    }
    return nativeUnitFrom(data, len);
}

// Web-stream startup. `maxFrameSize == 0` is not a threshold: the decoder has
// not published a block size yet, and a 1-byte fill must not open the stream.
inline bool webStreamStartupReady(bool decoder_ready, size_t filled, size_t maxFrameSize, bool all_data, size_t flac_unit) {
    if (all_data) return true;
    if (!decoder_ready) return false;
    if (flac_unit > 0) return true;
    if (maxFrameSize == 0) return false;
    return filled > maxFrameSize;
}

// Web-file startup after the header parser has finished. A finished body or
// one bounded FLAC unit is enough. Other codecs keep the two-block reserve.
inline bool webFileStartupReady(size_t filled, size_t maxBlock, bool all_data, size_t flac_unit) {
    if (all_data) return true;
    if (flac_unit > 0) return true;
    if (maxBlock == 0) return false;
    return filled > maxBlock && (filled - maxBlock) > maxBlock;
}

// Bytes the caller may feed to the decoder from this buffer. A bounded unit is
// only the first page or frame; bytes after it belong to a later call.
// A missing unit returns `len` so the capacity path is unchanged.
inline size_t flacApprovedBytes(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return 0;
    const size_t unit = flacContainedUnit(data, len);
    if (unit > 0 && unit <= len) return unit;
    return len;
}

// Offset of the next 0xFFF8 header after byte 0. 0 when this buffer starts on
// a header or no later header is present. Used to keep a split-output tail
// from including the following frame.
inline size_t flacBytesBeforeNextHeader(const uint8_t* data, size_t len) {
    using namespace flac_startup_detail;
    if (data == nullptr || len < 7) return 0;
    const size_t next = findFrameHeader(data, len, 1);
    if (next == static_cast<size_t>(-1) || next == 0) return 0;
    return next;
}

// How many bytes of this buffer the decoder may see. A proven prefix is a hard
// end. An unproven buffer keeps the whole length, which is the capacity path.
inline size_t flacDecodeWindow(const uint8_t* data, size_t len, bool split_output) {
    if (data == nullptr || len == 0) return 0;
    if (split_output) {
        if (flac_startup_detail::frameHeaderLen(data, len) != 0) return len;
        const size_t prefix = flacBytesBeforeNextHeader(data, len);
        if (prefix > 0 && prefix < len) return prefix;
        return len;
    }
    const size_t approved = flacApprovedBytes(data, len);
    if (approved > 0 && approved < len) return approved;
    return len;
}

// res == 100 is two decoder states that share a number.
// ogg_page: FLAC_PARSE_OGG_DONE. Parsing the page header consumes bytes.
// Arming the next segment returns 100 again and consumes nothing; that one
// idle step stays inside this page. A second idle 100 is a spin and stops.
// native: FLAC_DECODE_FRAMES_LOOP. The header consume already advanced, and
// the subframe pass must advance too. One native continuation is enough.
// Neither mode continues once `used` reaches the approved window.
inline bool flacDecodeContinue(int res, int used, int left, size_t approved, int spins, bool ogg_page, int prev_used) {
    if (res != 100) return false;
    if (left <= 0 || used < 0) return false;
    if (approved == 0 || static_cast<size_t>(used) >= approved) return false;
    if (spins > 0 && used < prev_used) return false;
    if (!ogg_page && spins > 0 && used <= prev_used) return false;
    if (ogg_page && spins > 1 && used <= prev_used) return false;
    const int budget = ogg_page ? 8 : 1;
    return spins < budget;
}
inline size_t decodeReadMinimum(size_t readable, size_t maxBlock, size_t flac_unit) {
    if (flac_unit > 0 && flac_unit <= readable && (maxBlock == 0 || flac_unit < maxBlock)) return flac_unit;
    return maxBlock;
}

// True when startup has seen no new payload bytes for longer than `timeout_ms`.
// A call that just received bytes is not stalled. This is not the running-stream
// underrun counter in streamDetection. Exactly `timeout_ms` is not stalled.
inline bool startupStalled(uint32_t now, uint32_t anchor, bool have_anchor, bool got_bytes, uint32_t timeout_ms) {
    if (!have_anchor || got_bytes) return false;
    return (now - anchor) > timeout_ms;
}

// AF-2.5 applies the startup stall only to FLAC, and only while the stream is
// still closed. A running stream keeps streamDetection. MP3 and AAC do not use
// this stop. all_data is a finished body, not a stall.
inline bool startupStallApplies(bool flac, bool stream_open, bool all_data, bool stalled) {
    return flac && !stream_open && !all_data && stalled;
}

} // namespace audiolib
