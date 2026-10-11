#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace geode::prelude;

static float rand01() {
    return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        float timer = 0.f;
        float interval = 1.0f;
        bool pressed = false;
        float nerves = 0.1f;   // 0.1 = tranquilo, 1.0 = maximo
        float phase = 0.f;     // para el latido
        CCLabelBMFont* hud = nullptr;
    };

    std::string bestKey() {
        return fmt::format("best-{}", m_level->m_levelID.value());
    }

    float bestPercent() {
        return Mod::get()->getSavedValue<float>(this->bestKey(), 0.f);
    }

    // Nervios segun la cercania al mejor progreso
    // Nervios segun la cercania al mejor progreso
    float targetNerves(float p, float best) {
        const float MIN_RECORD = 20.f;         // por debajo de este record no hay nervios
        if (best < MIN_RECORD) return 0.1f;
        if (p >= best) return 1.0f;            // zona desconocida: maximo
        float window = std::min(15.f, best * 0.5f); // desde cuantos % antes empieza a subir
        float d = best - p;
        float k = std::clamp(1.f - d / window, 0.f, 1.f);
        return 0.1f + 0.9f * k;
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->timer = 0.f;
        m_fields->interval = 1.0f;
        m_fields->pressed = false;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (!m_player1) return;

        bool botOn = Mod::get()->getSettingValue<bool>("bot-active");
        bool nervesOn = Mod::get()->getSettingValue<bool>("nerves-enabled");

        // --- Nervios y pulso ---
        float p = this->getCurrentPercent();
        float best = this->bestPercent();
        float target = (botOn && nervesOn) ? this->targetNerves(p, best) : 0.1f;
        m_fields->nerves += (target - m_fields->nerves) * std::min(1.f, dt * 0.4f);

        float bpm = 70.f + 110.f * m_fields->nerves;
        m_fields->phase += dt * (bpm / 60.f) * 6.2831853f;

        // --- Pantalla (HUD) ---
        if (!m_fields->hud) {
            m_fields->hud = CCLabelBMFont::create("BPM 70", "bigFont.fnt");
            auto win = CCDirector::get()->getWinSize();
            m_fields->hud->setPosition({win.width - 80.f, win.height - 25.f});
            this->addChild(m_fields->hud, 1000);
        }
        m_fields->hud->setVisible(botOn);
        m_fields->hud->setString(fmt::format("BPM {}", static_cast<int>(bpm)).c_str());
        m_fields->hud->setScale(0.5f + 0.08f * std::sin(m_fields->phase));
        GLubyte g = static_cast<GLubyte>(255.f * (1.f - m_fields->nerves));
        m_fields->hud->setColor(ccColor3B{255, g, g});

        // --- Bot ---
        if (!botOn) return;
        if (m_player1->m_isDead) return;

        m_fields->timer += dt;
        float n = nervesOn ? m_fields->nerves : 0.f;

        if (!m_fields->pressed && m_fields->timer >= m_fields->interval) {
            m_fields->timer = 0.f;
            // con nervios el ritmo se vuelve irregular
            m_fields->interval = 1.0f + (rand01() - 0.5f) * 0.6f * n;
            // a veces se le pasa el salto
            if (rand01() < n * 0.25f) return;
            this->handleButton(true, 1, true);
            m_fields->pressed = true;
        } else if (m_fields->pressed && m_fields->timer >= 0.1f) {
            this->handleButton(false, 1, true);
            m_fields->pressed = false;
            m_fields->timer = 0.f;
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        PlayLayer::destroyPlayer(player, obj);

        if (Mod::get()->getSettingValue<bool>("bot-active")) {
            float p = this->getCurrentPercent();
            if (p > this->bestPercent()) {
                Mod::get()->setSavedValue<float>(this->bestKey(), p);
            }
            log::info("El jugador murio en {}%", static_cast<int>(p));
        }
    }
};
