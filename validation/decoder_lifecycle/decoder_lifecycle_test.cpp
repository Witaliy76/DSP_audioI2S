// AF-2.6A concurrency review.
// Uses production helpers from src/decoder_lifecycle.h.
// Includes a real two-thread section with barriers. That is still not FreeRTOS
// and not a second ESP32 core.

#include "../../src/decoder_lifecycle.h"

#include <atomic>
#include <barrier>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_failed = 0;

void check(bool cond, const char* expr, int line) {
    if (cond) return;
    std::printf("FAIL %s:%d: %s\n", "decoder_lifecycle_test.cpp", line, expr);
    ++g_failed;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

using audiolib::DecoderCtrl;

struct World {
    std::atomic<uint8_t>  ctrl{0};
    std::atomic<uint32_t> stream_epoch{0};
    std::atomic<uint32_t> reconnect_epoch{0};
    bool                  audio_holds = false;
    bool                  decoder_alive = true;
    bool                  use_decoder = true;
    int                   destroys = 0;
    int                   gives = 0;
    int                   reconnects = 0;
    int                   decodes = 0;
    int                   skipped_stale = 0;

    void publish(DecoderCtrl request) {
        if (request == DecoderCtrl::Reconnect) {
            reconnect_epoch.store(stream_epoch.load(std::memory_order_acquire), std::memory_order_release);
        }
        audiolib::requestDecoderCtrl(ctrl, request);
    }

    bool pending() const { return audiolib::decoderCtrlPending(ctrl.load(std::memory_order_acquire)); }

    void audio_enter(bool take_ok) {
        const audiolib::DecodeGate gate = audiolib::decodeGate(take_ok, pending());
        if (!gate.enter) {
            if (gate.give) ++gives;
            return;
        }
        audio_holds = true;
        if (use_decoder && decoder_alive && !pending()) ++decodes;
    }

    void audio_fault(bool fatal, bool need_restart) {
        const audiolib::FaultAction act = audiolib::deferredFault(fatal, need_restart);
        CHECK(!act.call_stop_now);
        CHECK(!act.call_connect_now);
        CHECK(audio_holds);
        if (!act.keep_using_decoder) {
            publish(act.publish);
            use_decoder = false;
        }
    }

    void audio_bad_format(int channels, int bits) {
        CHECK(audio_holds);
        if (audiolib::sampleFormatUsable(channels, bits)) return;
        publish(DecoderCtrl::Stop);
        use_decoder = false;
    }

    void audio_leave() {
        CHECK(audio_holds);
        audio_holds = false;
        ++gives;
    }

    void bump_user_stream() { stream_epoch.fetch_add(1, std::memory_order_acq_rel); }

    // Public stopSong / setDefaults. clear_pending matches production.
    void external_stop(bool take_ok, bool clear_pending = true) {
        const audiolib::MutexStop decision = audiolib::mutexStop(take_ok && !audio_holds);
        if (!decision.proceed) {
            CHECK(!decision.give);
            publish(decision.publish);
            return;
        }
        if (clear_pending) ctrl.store(static_cast<uint8_t>(DecoderCtrl::None), std::memory_order_release);
        if (decoder_alive) {
            decoder_alive = false;
            ++destroys;
        }
        if (decision.give) ++gives;
        use_decoder = false;
    }

    // Production serviceDecoderCtrl shape: claim -> owned stop -> restore/reconnect.
    void service(bool take_ok) {
        uint8_t claimed = 0;
        if (!audiolib::claimDecoderCtrl(ctrl, &claimed)) return;

        const audiolib::MutexStop decision = audiolib::mutexStop(take_ok && !audio_holds);
        if (!decision.proceed) {
            CHECK(!decision.give);
            publish(decision.publish); // Stop from failed take
            audiolib::restoreDecoderCtrl(ctrl, claimed);
            return;
        }

        if (decoder_alive) {
            decoder_alive = false;
            ++destroys;
        }
        if (decision.give) ++gives;
        use_decoder = false;

        const bool do_reconnect = audiolib::reconnectAfterOwnedStop(ctrl, claimed) &&
                                  audiolib::reconnectEpochCurrent(reconnect_epoch.load(std::memory_order_acquire),
                                                                  stream_epoch.load(std::memory_order_acquire));
        if (do_reconnect) ++reconnects;
        else if (claimed == static_cast<uint8_t>(DecoderCtrl::Reconnect)) ++skipped_stale;
    }

    void restart() {
        CHECK(!audio_holds);
        CHECK(!pending());
        CHECK(!decoder_alive);
        decoder_alive = true;
        use_decoder = true;
        reconnects = 0;
        skipped_stale = 0;
    }
};

void expect_legacy() {
    const audiolib::LegacyStop stop = audiolib::legacyStopWhileDecodeHeld();
    const audiolib::LegacyStop restart = audiolib::legacyNeedRestartWhileDecodeHeld();
    CHECK(stop.destroyed_without_ownership);
    CHECK(stop.gave_without_ownership);
    CHECK(stop.tested_constant_not_result);
    CHECK(restart.reconnect_under_mutex);
    std::printf("PRE-FIX decode mutex held: destroy=1 give_without_ownership=1 constant_pdTRUE=1\n");
    std::printf("PRE-FIX MP3 NEED_RESTART: reconnect_under_mutex=1\n");
}

void scenario_stop_during_decode() {
    World w;
    w.audio_enter(true);
    w.audio_fault(true, false);
    CHECK(w.destroys == 0);
    w.service(true); // cannot take while held
    CHECK(w.destroys == 0);
    CHECK(w.ctrl.load() == static_cast<uint8_t>(DecoderCtrl::Stop));
    w.audio_leave();
    w.service(true);
    CHECK(w.destroys == 1);
    CHECK(!w.pending());
    w.service(true);
    CHECK(w.destroys == 1);
}

void scenario_take_fails_restore_keeps_stop() {
    World w;
    w.publish(DecoderCtrl::Reconnect);
    // Claim Reconnect, fail take (publishes Stop), restore must keep Stop.
    uint8_t claimed = 0;
    CHECK(audiolib::claimDecoderCtrl(w.ctrl, &claimed));
    CHECK(claimed == static_cast<uint8_t>(DecoderCtrl::Reconnect));
    audiolib::requestDecoderCtrl(w.ctrl, DecoderCtrl::Stop); // as failed take does
    audiolib::restoreDecoderCtrl(w.ctrl, claimed);
    CHECK(w.ctrl.load() == static_cast<uint8_t>(DecoderCtrl::Stop));
    w.service(true);
    CHECK(w.destroys == 1);
    CHECK(w.reconnects == 0);
}

void scenario_late_stop_cancels_reconnect() {
    World w;
    w.publish(DecoderCtrl::Reconnect);
    uint8_t claimed = 0;
    CHECK(audiolib::claimDecoderCtrl(w.ctrl, &claimed));
    CHECK(claimed == static_cast<uint8_t>(DecoderCtrl::Reconnect));
    // Destroy succeeds for the claim...
    w.decoder_alive = false;
    w.destroys = 1;
    // ...but Stop arrives before reconnect decision.
    w.publish(DecoderCtrl::Stop);
    CHECK(!audiolib::reconnectAfterOwnedStop(w.ctrl, claimed));
    CHECK(w.ctrl.load() == static_cast<uint8_t>(DecoderCtrl::None));
}

void scenario_user_station_change_stales_reconnect() {
    World w;
    w.publish(DecoderCtrl::Reconnect);
    w.bump_user_stream(); // setDefaults on new connecttohost
    w.service(true);
    CHECK(w.destroys == 1);
    CHECK(w.reconnects == 0);
    CHECK(w.skipped_stale == 1);
}

void scenario_need_restart_not_under_mutex() {
    World w;
    w.audio_enter(true);
    w.audio_fault(false, true);
    CHECK(w.reconnects == 0);
    w.audio_leave();
    w.service(true);
    CHECK(w.destroys == 1);
    CHECK(w.reconnects == 1);
}

void scenario_stop_outranks_reconnect() {
    World w;
    w.publish(DecoderCtrl::Reconnect);
    w.publish(DecoderCtrl::Stop);
    CHECK(w.ctrl.load() == static_cast<uint8_t>(DecoderCtrl::Stop));
    w.publish(DecoderCtrl::Reconnect);
    CHECK(w.ctrl.load() == static_cast<uint8_t>(DecoderCtrl::Stop));
    w.service(true);
    CHECK(w.destroys == 1);
    CHECK(w.reconnects == 0);
}

void scenario_dual_service_one_destroy() {
    World w;
    w.publish(DecoderCtrl::Stop);
    w.service(true);
    w.service(true);
    CHECK(w.destroys == 1);
}

void scenario_idle_stop_and_restart() {
    World w;
    w.external_stop(true);
    CHECK(w.destroys == 1);
    w.restart();
    w.audio_enter(true);
    CHECK(w.decodes == 1);
    w.audio_leave();
}

// Real two OS threads. Barriers force the reviewed interleavings.
void threaded_stop_not_lost_to_reconnect_restore() {
    std::atomic<uint8_t> slot{static_cast<uint8_t>(DecoderCtrl::Reconnect)};
    std::barrier         sync(2);

    std::thread audio([&] {
        sync.arrive_and_wait(); // 1: both ready
        // After service claims Reconnect and fails take, audio publishes Stop.
        sync.arrive_and_wait(); // 2: claim done, about to fail/restore
        audiolib::requestDecoderCtrl(slot, DecoderCtrl::Stop);
        sync.arrive_and_wait(); // 3: Stop published before restore returns
    });

    std::thread loop([&] {
        sync.arrive_and_wait(); // 1
        uint8_t claimed = 0;
        CHECK(audiolib::claimDecoderCtrl(slot, &claimed));
        CHECK(claimed == static_cast<uint8_t>(DecoderCtrl::Reconnect));
        sync.arrive_and_wait(); // 2: allow audio to publish Stop
        // Simulate failed take publish + restore (production order).
        audiolib::requestDecoderCtrl(slot, DecoderCtrl::Stop);
        audiolib::restoreDecoderCtrl(slot, claimed);
        sync.arrive_and_wait(); // 3
        CHECK(slot.load() == static_cast<uint8_t>(DecoderCtrl::Stop));
    });

    audio.join();
    loop.join();
}

void threaded_dual_claim_single_winner() {
    std::atomic<uint8_t> slot{static_cast<uint8_t>(DecoderCtrl::Stop)};
    std::atomic<int>     wins{0};
    std::barrier         sync(2);

    auto worker = [&] {
        sync.arrive_and_wait();
        uint8_t claimed = 0;
        if (audiolib::claimDecoderCtrl(slot, &claimed)) wins.fetch_add(1);
    };

    std::thread a(worker);
    std::thread b(worker);
    a.join();
    b.join();
    CHECK(wins.load() == 1);
    CHECK(slot.load() == static_cast<uint8_t>(DecoderCtrl::None));
}

void threaded_late_stop_blocks_reconnect() {
    std::atomic<uint8_t> slot{static_cast<uint8_t>(DecoderCtrl::Reconnect)};
    std::barrier         sync(2);
    std::atomic<bool>    reconnect_allowed{true};

    std::thread publisher([&] {
        sync.arrive_and_wait(); // after claim
        audiolib::requestDecoderCtrl(slot, DecoderCtrl::Stop);
        sync.arrive_and_wait();
    });

    std::thread service([&] {
        uint8_t claimed = 0;
        CHECK(audiolib::claimDecoderCtrl(slot, &claimed));
        sync.arrive_and_wait();
        sync.arrive_and_wait(); // Stop published
        reconnect_allowed.store(audiolib::reconnectAfterOwnedStop(slot, claimed));
    });

    publisher.join();
    service.join();
    CHECK(!reconnect_allowed.load());
    CHECK(slot.load() == static_cast<uint8_t>(DecoderCtrl::None));
}

} // namespace

int main() {
    expect_legacy();
    scenario_stop_during_decode();
    scenario_take_fails_restore_keeps_stop();
    scenario_late_stop_cancels_reconnect();
    scenario_user_station_change_stales_reconnect();
    scenario_need_restart_not_under_mutex();
    scenario_stop_outranks_reconnect();
    scenario_dual_service_one_destroy();
    scenario_idle_stop_and_restart();

    threaded_stop_not_lost_to_reconnect_restore();
    threaded_dual_claim_single_winner();
    threaded_late_stop_blocks_reconnect();

    if (g_failed) {
        std::printf("decoder_lifecycle: FAIL %d\n", g_failed);
        return 1;
    }
    std::printf("decoder_lifecycle: PASS\n");
    std::printf("host model: production decoder_lifecycle.h; two std::thread cases with barriers; not FreeRTOS\n");
    return 0;
}
