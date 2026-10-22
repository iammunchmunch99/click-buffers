#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

class $modify(BufferGJBGL, GJBaseGameLayer) {
    struct Fields {
        double time = 0.0;
        double pressTime[2] = {-1e9, -1e9};
        bool releaseDeferred[2] = {false, false};
        bool landedWhileDeferred[2] = {false, false};
    };

    // ---- helpers -------------------------------------------------------
    bool bufferActive() {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return false;
        if (mod->getSettingValue<bool>("disable-on-rated") &&
            m_level && m_level->m_stars.value() > 0) return false;
        return true;
    }

    double windowSeconds() {
        return Mod::get()->getSettingValue<int64_t>("buffer-frames") / 60.0;
    }

    bool modeAllowed(PlayerObject* p) {
        if (!Mod::get()->getSettingValue<bool>("cube-like-only")) return true;
        return !(p->m_isShip || p->m_isBird || p->m_isBall ||
                 p->m_isDart || p->m_isSwing);
    }

    void clearState(int i) {
        auto f = m_fields.self();
        f->releaseDeferred[i] = false;
        f->landedWhileDeferred[i] = false;
    }

    // ---- input ---------------------------------------------------------
    void handleButton(bool down, int button, bool isPlayer1) {
        // button 1 = jump
        if (button != 1 || !bufferActive()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        int i = isPlayer1 ? 0 : 1;
        if (i == 1 && !Mod::get()->getSettingValue<bool>("affect-player-2")) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto f = m_fields.self();
        auto player = isPlayer1 ? m_player1 : m_player2;

        if (down) {
            f->pressTime[i] = f->time;
            clearState(i);
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // Release: if the player is airborne and the press was recent,
        // keep the "hold" alive until landing so the jump fires.
        bool recent = (f->time - f->pressTime[i]) <= windowSeconds();
        if (player && !player->m_isOnGround && recent && modeAllowed(player)) {
            f->releaseDeferred[i] = true;
            f->landedWhileDeferred[i] = false;
            return; // swallow the release for now
        }

        clearState(i);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // ---- per physics step ---------------------------------------------
     void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto f = m_fields.self();
        f->time += dt;
                GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        for (int i = 0; i < 2; i++) {
            if (!f->releaseDeferred[i]) continue;
            auto player = i == 0 ? m_player1 : m_player2;

            bool timedOut = (f->time - f->pressTime[i]) > windowSeconds();
            bool dead = !player || player->m_isDead;

            if (player && player->m_isOnGround) f->landedWhileDeferred[i] = true;
            bool jumped = f->landedWhileDeferred[i] && !player->m_isOnGround;

            if (dead || timedOut || jumped) {
                clearState(i);
                GJBaseGameLayer::handleButton(false, 1, i == 0);
            }
        }
    }
};
