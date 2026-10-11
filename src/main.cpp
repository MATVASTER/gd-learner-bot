#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

class $modify(BotPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (!m_player1) return;

        float x = m_player1->getPositionX();
        float y = m_player1->getPositionY();

        log::info("x={} y={}", x, y);
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        PlayLayer::destroyPlayer(player, obj);
        log::info("El jugador murio");
    }
};
