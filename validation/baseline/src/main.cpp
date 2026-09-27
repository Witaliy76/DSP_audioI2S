// AF-1.1 control sketch for the local DSP_audioI2S tree.
// RU: Проверяет сборку текущих исходников на ESP32-S3 и сигнатуры публичного Audio API,
//     callback и обеих weak PCM-функций. Прошивка и сеть здесь не запускаются.
// EN: Checks that the current sources build on ESP32-S3, including the public Audio API,
//     the callback, and both weak PCM hooks. This sketch is not flashed and opens no network.

#include <Arduino.h>
#include <Audio.h>

namespace {

Audio audio;

void onAudioInfo(Audio::msg_t message) {
    (void)message;
}

float identityVolumeCurve(float position) {
    return position;
}

}  // namespace

void audio_process_raw_samples(int32_t* outBuff, int16_t validSamples) {
    (void)outBuff;
    (void)validSamples;
}

void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S) {
    (void)outBuff;
    (void)validSamples;
    if (continueI2S != nullptr) {
        *continueI2S = true;
    }
}

void setup() {
    Audio::audio_info_callback = onAudioInfo;
    audio.setVolumeCurve(identityVolumeCurve);
    audio.setPinout(4, 5, 6, 7);
    audio.setVolume(12, 0);
    audio.setTone(0.0f, 0.0f, 0.0f);
    // Reserved placeholder. Not a credential and not a live stream.
    audio.connecttohost("http://example.invalid/baseline.mp3");

    int32_t samples[2] = {};
    bool continueI2S = true;
    audio_process_raw_samples(samples, 1);
    audio_process_i2s(samples, 1, &continueI2S);
}

void loop() {
    audio.loop();
}
