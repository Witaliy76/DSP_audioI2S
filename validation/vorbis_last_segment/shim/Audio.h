#pragma once
// Host stand-in for src/Audio.h. Not part of the firmware build.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

class Audio {
  public:
    enum { evt_id3data = 1, evt_name = 2 };
    template <typename... Args>
    static void AUDIO_LOG_IMPL(uint8_t, const char*, int, const char*, const char*, Args&&...) {}
    template <typename... Args> void info(Audio&, int, const char*, Args&&...) {}
};

class Decoder {
  public:
    Decoder() = default;
    explicit Decoder(Audio&) {}
    virtual ~Decoder() = default;
    virtual bool init(size_t outBuffSize) = 0;
    virtual void clear() = 0;
    virtual void reset() = 0;
    virtual bool isValid() = 0;
    virtual int32_t findSyncWord(uint8_t* buf, int32_t nBytes) = 0;
    virtual uint8_t getChannels() = 0;
    virtual uint32_t getSampleRate() = 0;
    virtual uint8_t getBitsPerSample() = 0;
    virtual uint32_t getBitRate() = 0;
    virtual uint32_t getAudioDataStart() = 0;
    virtual uint32_t getAudioFileDuration() = 0;
    virtual uint32_t getOutputSamples() = 0;
    virtual const char* getStreamTitle() = 0;
    virtual const char* whoIsIt() = 0;
    virtual int32_t decode(uint8_t* inbuf, int32_t* bytesLeft, int32_t* outbuf) = 0;
    virtual void setRawBlockParams(uint8_t, uint32_t, uint8_t, uint32_t, uint32_t) = 0;
    virtual std::vector<uint32_t> getMetadataBlockPicture() = 0;
    virtual const char* arg1() = 0;
    virtual const char* arg2() = 0;
    virtual int32_t val1() = 0;
    virtual int32_t val2() = 0;
};
