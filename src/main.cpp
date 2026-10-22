#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <chrono>
#include <vector>

using namespace geode::prelude;

// STAGE 2: predictive input buffer.
// When you click, the mod saves the game state, then tries delays of 0,1,2...
// up to "Max Delay" ticks. For each, it simulates a few ticks ahead and checks
// whether the player dies. It uses the smallest delay that survives. If none
// survive, your original click goes through unchanged.
// Ticks are physics ticks (240 per second), so 4 ticks = 1 frame at 60 FPS.

static bool g_simulating = false;  // true while we are testing a delay
static bool g_simDied = false;     // set if the player "died" during a test

struct PendingInput {
    bool down;
    int button;
    bool isPlayer1;
    uint64_t due;
};

// While testing, stop deaths from really happening and just record them.
class $modify(SimPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_simulating) {
            g_simDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(BufferGJBGL, GJBaseGameLayer) {
    struct Fields {
        uint64_t tick = 0;
        std::vector<PendingInput> pending;
        uint64_t lastDue[2] = {0, 0};
        bool deciding = false;
        uint64_t calls = 0;
        std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
    };

    // ---- settings ------------------------------------------------------
    bool bufferOn()   { return Mod::get()->getSettingValue<bool>("enabled"); }
    bool debugOn()    { return Mod::get()->getSettingValue<bool>("debug-log"); }
    bool predictOn()  { return Mod::get()->getSettingValue<bool>("predict"); }
    bool testMode()   { return Mod::get()->getSettingValue<bool>("test-fixed-delay"); }

    uint64_t maxDelayTicks() {
        auto v = Mod::get()->getSettingValue<int64_t>("max-delay-ticks");
        return v < 0 ? 0 : static_cast<uint64_t>(v);
    }
    uint64_t lookaheadTicks() {
        auto v = Mod::get()->getSettingValue<int64_t>("lookahead-ticks");
        return v < 1 ? 1 : static_cast<uint64_t>(v);
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

    // ---- one trial: apply the input after `delay` ticks, run ahead ------
    // returns 1 = survived, 0 = died, -1 = simulation did not advance
    int trial(PlayLayer* pl, CheckpointObject* cp, bool down, int button,
              bool isPlayer1, uint64_t delay, uint64_t look) {
        auto f = m_fields.self();
        auto player = isPlayer1 ? m_player1 : m_player2;

        g_simulating = true;
        g_simDied = false;

        float x0 = player->getPositionX();
        uint64_t total = delay + look;

        for (uint64_t t = 0; t < total && !g_simDied; t++) {
            // inputs we already queued earlier still need to land in the test
            for (auto& p : f->pending) {
                if (p.due >= f->tick && p.due - f->tick == t) {
                    GJBaseGameLayer::handleButton(p.down, p.button, p.isPlayer1);
                }
            }
            if (t == delay) {
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
            }
            GJBaseGameLayer::update(1.f / 240.f);
        }

        float x1 = player->getPositionX();
        bool died = g_simDied || player->m_isDead;

        g_simulating = false;
        g_simDied = false;

        pl->loadFromCheckpoint(cp);  // put everything back

        if (x1 == x0 && !died) return -1;  // nothing moved: test is not valid
        return died ? 0 : 1;
    }

    // ---- pick the smallest surviving delay ------------------------------
    uint64_t chooseDelay(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        auto player = isPlayer1 ? m_player1 : m_player2;
        if (!pl || !player) return 0;

        uint64_t maxD = maxDelayTicks();
        uint64_t look = lookaheadTicks();

        auto cp = pl->createCheckpoint();
        if (!cp) return 0;
        cp->retain();

        uint64_t chosen = 0;
        bool found = false;
        bool invalid = false;

        for (uint64_t d = 0; d <= maxD; d++) {
            int r = trial(pl, cp, down, button, isPlayer1, d, look);
            if (r < 0) { invalid = true; break; }
            if (r == 1) { chosen = d; found = true; break; }
        }
        cp->release();

        if (debugOn()) {
            if (invalid) {
                log::warn("[ClickBuffer] SIMULATION DID NOT ADVANCE - prediction skipped");
            } else if (found && chosen > 0) {
                log::info("[ClickBuffer] *** SAVED +{} TICKS ({:.2f} frames @ 60fps) {} P{} [limit {}] ***",
                          chosen, chosen / 4.0, down ? "PRESS" : "RELEASE",
                          isPlayer1 ? 1 : 2, maxD);
            } else if (!found) {
                log::info("[ClickBuffer] no surviving delay up to {} ticks, keeping original", maxD);
            }
        }
        return found ? chosen : 0;
    }

    // ---- input ---------------------------------------------------------
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_simulating || button != 1 || !bufferOn()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto f = m_fields.self();
        int i = isPlayer1 ? 0 : 1;
        auto player = isPlayer1 ? m_player1 : m_player2;

        uint64_t delay = 0;
        // Wave reacts to presses AND releases. Other modes only to presses.
        bool relevant = affects(player) && (down || (player && player->m_isDart));

        if (relevant && !f->deciding) {
            f->deciding = true;
            if (predictOn()) {
                delay = chooseDelay(down, button, isPlayer1);
            } else if (testMode()) {
                delay = maxDelayTicks();
            }
            f->deciding = false;
        }

        // never let an input jump ahead of an earlier one from the same player
        uint64_t due = std::max<uint64_t>(f->tick + delay, f->lastDue[i]);

        if (due <= f->tick && !hasPendingFor(isPlayer1)) {
            f->lastDue[i] = f->tick;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        f->lastDue[i] = due;
        f->pending.push_back({down, button, isPlayer1, due});
    }

    // ---- once per physics tick ----------------------------------------
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_simulating) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        auto f = m_fields.self();
        f->tick++;

        if ((m_player1 && m_player1->m_isDead) || (m_player2 && m_player2->m_isDead)) {
            f->pending.clear();
        }

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
