#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace geode::prelude;

static float rand01() {
    return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

// ---------- Conexion con Python ----------
static SOCKET g_sock = INVALID_SOCKET;
static bool g_wsaReady = false;
static float g_retryTimer = 0.f;

static void closeSock() {
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
}

static bool connectToPython() {
    if (g_sock != INVALID_SOCKET) return true;
    if (!g_wsaReady) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        g_wsaReady = true;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5555);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return false;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    g_sock = s;
    log::info("Conectado a Python");
    return true;
}

// Devuelve -1 si fallo la comunicacion, o la accion (0 = soltar, 1 = mantener)
static int askPython(const std::string& state) {
    std::string msg = state + "\n";
    if (send(g_sock, msg.c_str(), static_cast<int>(msg.size()), 0) == SOCKET_ERROR) {
        closeSock();
        return -1;
    }
    char buf[8];
    int n = recv(g_sock, buf, sizeof(buf), 0);
    if (n <= 0) {
        closeSock();
        return -1;
    }
    return buf[0] == '1' ? 1 : 0;
}

// ---------- Mod ----------
class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        bool pressed = false;
        int delay = -1;        // retraso por nervios (frames), -1 = sin calcular
        float nerves = 0.1f;
        float phase = 0.f;
        CCLabelBMFont* hud = nullptr;
    };

    std::string bestKey() {
        return fmt::format("best-{}", m_level->m_levelID.value());
    }

    float bestPercent() {
        return Mod::get()->getSavedValue<float>(this->bestKey(), 0.f);
    }

    float targetNerves(float p, float best) {
        const float MIN_RECORD = 20.f;
        if (best < MIN_RECORD) return 0.1f;
        if (p >= best) return 1.0f;
        float window = std::min(15.f, best * 0.5f);
        float d = best - p;
        float k = std::clamp(1.f - d / window, 0.f, 1.f);
        return 0.1f + 0.9f * k;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->pressed = false;
        m_fields->delay = -1;
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

        if (!botOn) return;

        // --- Conectar con Python (reintenta cada 2 segundos) ---
        if (g_sock == INVALID_SOCKET) {
            g_retryTimer -= dt;
            if (g_retryTimer > 0.f) return;
            g_retryTimer = 2.f;
            if (!connectToPython()) return;
        }

        // --- Enviar estado y recibir accion ---
        bool dead = m_player1->m_isDead;
        float y = m_player1->getPositionY();
        float yv = static_cast<float>(m_player1->m_yVelocity);
        int ground = m_player1->m_isOnGround ? 1 : 0;

        std::string state = fmt::format("{:.4f},{:.2f},{:.3f},{},{}", p, y, yv, ground, dead ? 1 : 0);
        int raw = askPython(state);
        if (raw < 0) return;

        if (dead) {
            m_fields->pressed = false;
            m_fields->delay = -1;
            return;
        }

        // --- Aplicar la accion (con retraso si esta nervioso) ---
        int action = raw;
        float n = nervesOn ? m_fields->nerves : 0.f;

        if (raw == 0) m_fields->delay = -1;
        if (action == 1 && !m_fields->pressed) {
            if (m_fields->delay < 0) {
                m_fields->delay = (rand01() < n * 0.35f) ? 2 + (std::rand() % 8) : 0;
            }
            if (m_fields->delay > 0) {
                m_fields->delay--;
                action = 0;
            }
        }

        if (action == 1 && !m_fields->pressed) {
            this->handleButton(true, 1, true);
            m_fields->pressed = true;
            m_fields->delay = -1;
        } else if (action == 0 && m_fields->pressed) {
            this->handleButton(false, 1, true);
            m_fields->pressed = false;
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        PlayLayer::destroyPlayer(player, obj);

        if (Mod::get()->getSettingValue<bool>("bot-active")) {
            float p = this->getCurrentPercent();
            if (p > this->bestPercent()) {
                Mod::get()->setSavedValue<float>(this->bestKey(), p);
            }
        }
    }
};
