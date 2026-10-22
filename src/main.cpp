#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <chrono>
#include <vector>

using namespace geode::prelude;

// STAGE 1: tick-based delay plumbing.
// Clicks can be held back by N physics ticks and then released into the game.
// In test mode every affected click gets the same fixed delay, so you can
// confirm the mechanism works before the lookahead (stage 2) decides the delay.

struct PendingInput {
    bool down;
    int button;
    bool isPlayer1;
    uint64_t due;
};

class $modify(BufferGJBGL, GJBaseGameLayer) {
    struct Fields {
        uint64_t tick = 0;
        std::vector<PendingInput> pending;
        uint64_t lastDelay[2] = {0, 0};
        uint64_t calls = 0;
        std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
    };

    // ---- settings helpers ----------------------------------------------
    bool bufferOn() {
        return Mod::get()->getSettingValue<bool>("enabled");
    }

    bool debugOn() {
        return Mod::get()->getSettingValue<bool>("debug-log");
    }

    uint64_t maxDelayTicks() {
        auto v = Mod::get()->getSettingValue<int64_t>("max-delay-ticks");
        return v < 0 ? 0 : static_cast<uint64_t>(v);
    }

    bool affects(PlayerObject* p) {
        if (!p) return false;
        if (Mod::get()->getSettingValue<bool>("wave-only")) return p->m_isDart;
        return true;
    }

    bool hasPendingFor(bool isPlayer1) {
        for (auto& p : m_fields->pending) {
            if (p.isPlayer1 == isPlayer1) return true;
        }
        return false;
    }

    // ---- input ---------------------------------------------------------
    void handleButton(bool down, int button, bool isPlayer1) {
        // only touch the jump button, and only when the mod is on
        if (button != 1 || !bufferOn()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto f = m_fields.self();
        int i = isPlayer1 ? 0 : 1;
        auto player = isPlayer1 ? m_player1 : m_player2;

        if (down) {
            uint64_t delay = 0;
            bool testMode = Mod::get()->getSettingValue<bool>("test-fixed-delay");
            if (testMode && affects(player)) delay = maxDelayTicks();
            // STAGE 2 will replace the line above with the lookahead result.
            f->lastDelay[i] = delay;
        }

        // A release uses the same delay as its press so the order never flips.
        uint64_t delay = f->lastDelay[i];

        if (delay == 0 && !hasPendingFor(isPlayer1)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        f->pending.push_back({down, button, isPlayer1, f->tick + delay});

        if (debugOn()) {
            log::info("[ClickBuffer] {} P{} delayed by {} ticks ({} frames @ 60fps)",
                      down ? "PRESS" : "RELEASE", i + 1, delay, delay / 4.0);
        }
    }

    // ---- once per physics tick ----------------------------------------
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto f = m_fields.self();
        f->tick++;

        // Dead players: throw away anything still waiting.
        if ((m_player1 && m_player1->m_isDead) || (m_player2 && m_player2->m_isDead)) {
            f->pending.clear();
        }

        // Release inputs that are due, in order.
        for (size_t n = 0; n < f->pending.size();) {
            auto p = f->pending[n];
            if (p.due <= f->tick) {
                f->pending.erase(f->pending.begin() + n);
                GJBaseGameLayer::handleButton(p.down, p.button, p.isPlayer1);
            } else {
                n++;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        // Debug: report how many ticks per second this function really runs.
        if (debugOn()) {
            f->calls++;
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - f->lastReport).count();
            if (secs >= 5.0) {
                log::info("[ClickBuffer] processCommands calls per second: {:.1f}", f->calls / secs);
                f->calls = 0;
                f->lastReport = now;
            }
        }
    }
};
