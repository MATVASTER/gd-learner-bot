#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

static float rand01() {
    return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

static float clampv(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

struct Seg {
    float bottom;
    float top;
};

// Categorias de objetos especiales (por ID de objeto del juego)
// 1 = pad de salto, 2 = orb, 3 = gravedad (pad/orb azul), 4 = portal de modo,
// 5 = portal de gravedad, 6 = portal de velocidad/tamano/dual/espejo
static int specialCategory(int id) {
    switch (id) {
        case 35: case 140: case 1332:
            return 1;
        case 36: case 141: case 1333: case 1022: case 1330: case 1704: case 1751:
            return 2;
        case 67: case 84:
            return 3;
        case 12: case 13: case 47: case 111: case 660: case 745: case 1331:
            return 4;
        case 10: case 11:
            return 5;
        case 99: case 101: case 200: case 201: case 202: case 203: case 1334:
        case 286: case 287: case 45: case 46:
            return 6;
        default:
            return 0;
    }
}

// ---------- Rotacion de niveles ----------
static int g_deathsOnLevel = 0;
static bool g_switchPending = false;

static std::vector<int> parseLevels(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        try {
            int v = std::stoi(tok);
            if (v > 0) out.push_back(v);
        } catch (...) {}
    }
    return out;
}

static void goToLevel(int id) {
    auto level = LevelTools::getLevel(id, false);
    if (!level) {
        log::warn("No se pudo cargar el nivel {}", id);
        return;
    }
    auto scene = PlayLayer::scene(level, false, false);
    CCDirector::get()->replaceScene(CCTransitionFade::create(0.4f, scene));
}

static void nextLevel(int currentId) {
    static bool seeded = false;
    if (!seeded) {
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        seeded = true;
    }

    g_deathsOnLevel = 0;
    g_switchPending = false;
    int target = currentId;
    if (Mod::get()->getSettingValue<bool>("rotate-levels")) {
        auto list = parseLevels(Mod::get()->getSettingValue<std::string>("levels"));
        if (!list.empty()) {
            if (Mod::get()->getSettingValue<bool>("random-levels") && list.size() > 1) {
                // al azar, sin repetir el nivel actual
                do {
                    target = list[std::rand() % list.size()];
                } while (target == currentId);
            } else {
                auto it = std::find(list.begin(), list.end(), currentId);
                size_t idx = (it == list.end()) ? 0 : ((it - list.begin() + 1) % list.size());
                target = list[idx];
            }
        }
    }
    log::info("Cambiando al nivel {}", target);
    goToLevel(target);
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
    DWORD timeout = 120000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    g_sock = s;
    log::info("Conectado a Python");
    return true;
}

// Devuelve -1 si fallo la comunicacion, o la accion (0 = soltar, 1 = mantener)
static int askPython(const std::string& state) {
    std::string msg = state + "\n";
    if (send(g_sock, msg.c_str(), static_cast<int>(msg.size()), 0) == SOCKET_ERROR) {
        log::warn("Fallo al enviar a Python (error {})", WSAGetLastError());
        closeSock();
        return -1;
    }
    char buf[8];
    int n = recv(g_sock, buf, sizeof(buf), 0);
    if (n <= 0) {
        log::warn("Python no respondio (n={}, error {})", n, WSAGetLastError());
        closeSock();
        return -1;
    }
    return buf[0] == '1' ? 1 : 0;
}

// ---------- Mod ----------
class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        bool pressed = false;
        int delay = -1;
        float nerves = 0.1f;
        float phase = 0.f;
        CCLabelBMFont* hud = nullptr;

        // objetos tal como se cargan
        std::vector<GameObject*> rawHazards;
        std::vector<GameObject*> rawSolids;
        std::vector<GameObject*> rawSpecials;

        // mapa del nivel por columnas de 30 unidades
        std::vector<std::vector<Seg>> colSolids;
        std::vector<std::vector<float>> colHazards;
        std::vector<float> colSpec;      // columnas * 6: altura del especial, o -99999
        std::vector<float> clusterX;     // inicio de cada grupo de pinchos
        int cols = 0;
        bool eyesReady = false;
    };

    void addObject(GameObject* obj) {
        PlayLayer::addObject(obj);
        if (!obj) return;
        if (specialCategory(obj->m_objectID) > 0) {
            m_fields->rawSpecials.push_back(obj);
        } else if (obj->m_objectType == GameObjectType::Hazard) {
            m_fields->rawHazards.push_back(obj);
        } else if (obj->m_objectType == GameObjectType::Solid) {
            m_fields->rawSolids.push_back(obj);
        }
    }

    void prepareEyes() {
        float maxX = 0.f;
        for (auto o : m_fields->rawSolids) maxX = std::max(maxX, o->getPositionX());
        for (auto o : m_fields->rawHazards) maxX = std::max(maxX, o->getPositionX());
        for (auto o : m_fields->rawSpecials) maxX = std::max(maxX, o->getPositionX());
        int cols = static_cast<int>(maxX / 30.f) + 80;
        m_fields->cols = cols;
        m_fields->colSolids.assign(cols, std::vector<Seg>());
        m_fields->colHazards.assign(cols, std::vector<float>());
        m_fields->colSpec.assign(cols * 6, -99999.f);
        m_fields->clusterX.clear();

        for (auto o : m_fields->rawSolids) {
            float x = o->getPositionX();
            float y = o->getPositionY();
            float w = std::fabs(o->getContentSize().width * o->getScaleX());
            float h = std::fabs(o->getContentSize().height * o->getScaleY()) * 0.5f;
            int c0 = static_cast<int>((x - w * 0.5f + 1.f) / 30.f);
            int c1 = static_cast<int>((x + w * 0.5f - 1.f) / 30.f);
            for (int c = std::max(c0, 0); c <= c1 && c < cols; ++c) {
                m_fields->colSolids[c].push_back({y - h, y + h});
            }
        }
        for (auto o : m_fields->rawHazards) {
            int c = static_cast<int>(o->getPositionX() / 30.f);
            if (c < 0 || c >= cols) continue;
            m_fields->colHazards[c].push_back(o->getPositionY());
        }
        for (auto o : m_fields->rawSpecials) {
            int c = static_cast<int>(o->getPositionX() / 30.f);
            int cat = specialCategory(o->m_objectID);
            if (c < 0 || c >= cols || cat <= 0) continue;
            float& slot = m_fields->colSpec[c * 6 + (cat - 1)];
            if (slot < -90000.f) slot = o->getPositionY();
        }
        bool prev = false;
        for (int c = 0; c < cols; ++c) {
            bool has = !m_fields->colHazards[c].empty();
            if (has && !prev) m_fields->clusterX.push_back(c * 30.f);
            prev = has;
        }
        m_fields->eyesReady = true;
        log::info("Ojos listos: {} columnas, {} peligros, {} bloques, {} especiales",
                  cols, m_fields->rawHazards.size(), m_fields->rawSolids.size(),
                  m_fields->rawSpecials.size());
    }

    // status: 0 = jugando, 1 = muerto, 2 = nivel completado
    std::string makeState(int status) {
        float p = this->getCurrentPercent();
        float px = m_player1->getPositionX();
        float y = m_player1->getPositionY();
        float yv = static_cast<float>(m_player1->m_yVelocity);
        int ground = m_player1->m_isOnGround ? 1 : 0;
        int upside = m_player1->m_isUpsideDown ? 1 : 0;
        int mini = m_player1->m_vehicleSize < 0.9f ? 1 : 0;
        float speed = static_cast<float>(m_player1->m_playerSpeed);
        int mode = 0;
        if (m_player1->m_isShip) mode = 1;
        else if (m_player1->m_isBall) mode = 2;
        else if (m_player1->m_isBird) mode = 3;
        else if (m_player1->m_isDart) mode = 4;
        else if (m_player1->m_isRobot) mode = 5;
        else if (m_player1->m_isSpider) mode = 6;

        int levelId = static_cast<int>(m_level->m_levelID.value());
        int passed = static_cast<int>(
            std::lower_bound(m_fields->clusterX.begin(), m_fields->clusterX.end(), px) -
            m_fields->clusterX.begin());

        std::string s = fmt::format("{},{:.4f},{},{},{:.2f},{:.3f},{},{},{},{:.2f},{}",
                                    status, p, levelId, passed, y, yv, ground, upside, mini, speed, mode);

        int c0 = static_cast<int>(px / 30.f);
        for (int k = 0; k < 16; k++) {
            int c = c0 + k;
            float below = 90.f;
            float above = y + 999.f;
            int block = 0;
            int haz = 0;
            float hazRel = 0.f;
            float specRel = 0.f;
            float bestHaz = 1e9f;
            float bestSpec = 1e9f;
            int flags[6] = {0, 0, 0, 0, 0, 0};

            if (c >= 0 && c < m_fields->cols) {
                for (auto& sg : m_fields->colSolids[c]) {
                    if (sg.bottom < y + 12.f && sg.top > y - 12.f) {
                        block = 1;
                    } else if (sg.top <= y - 12.f) {
                        below = std::max(below, sg.top);
                    } else {
                        above = std::min(above, sg.bottom);
                    }
                }
                for (float hy : m_fields->colHazards[c]) {
                    haz = 1;
                    float d = std::fabs(hy - y);
                    if (d < bestHaz) {
                        bestHaz = d;
                        hazRel = clampv((hy - y) / 60.f, -3.f, 3.f);
                    }
                }
                for (int j = 0; j < 6; j++) {
                    float sy = m_fields->colSpec[c * 6 + j];
                    if (sy > -90000.f) {
                        flags[j] = 1;
                        float d = std::fabs(sy - y);
                        if (d < bestSpec) {
                            bestSpec = d;
                            specRel = clampv((sy - y) / 60.f, -3.f, 3.f);
                        }
                    }
                }
            }
            float relBelow = clampv((below - y) / 60.f, -3.f, 0.f);
            float relAbove = clampv((above - y) / 60.f, 0.f, 3.f);
            s += fmt::format(",{:.2f},{:.2f},{},{},{:.2f},{},{},{},{},{},{},{:.2f}",
                             relBelow, relAbove, block, haz, hazRel,
                             flags[0], flags[1], flags[2], flags[3], flags[4], flags[5], specRel);
        }
        return s;
    }

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
        if (!m_fields->eyesReady) this->prepareEyes();

        // --- Conectar con Python (reintenta cada 2 segundos) ---
        if (g_sock == INVALID_SOCKET) {
            g_retryTimer -= dt;
            if (g_retryTimer > 0.f) return;
            g_retryTimer = 2.f;
            if (!connectToPython()) return;
        }

        bool dead = m_player1->m_isDead;
        int raw = askPython(this->makeState(dead ? 1 : 0));
        if (raw < 0) return;

        if (dead) {
            m_fields->pressed = false;
            m_fields->delay = -1;
            if (g_switchPending) {
                nextLevel(static_cast<int>(m_level->m_levelID.value()));
            }
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

    void levelComplete() {
        bool botOn = Mod::get()->getSettingValue<bool>("bot-active");
        if (!botOn || !m_player1 || g_sock == INVALID_SOCKET || !m_fields->eyesReady) {
            PlayLayer::levelComplete();
            return;
        }
        // Se avisa a Python de que gano y se pasa al siguiente nivel (sin pantalla de victoria)
        askPython(this->makeState(2));
        nextLevel(static_cast<int>(m_level->m_levelID.value()));
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        PlayLayer::destroyPlayer(player, obj);
        if (!Mod::get()->getSettingValue<bool>("bot-active")) return;

        float p = this->getCurrentPercent();
        if (p > this->bestPercent()) {
            Mod::get()->setSavedValue<float>(this->bestKey(), p);
        }
        g_deathsOnLevel++;
        int limit = static_cast<int>(Mod::get()->getSettingValue<int64_t>("attempts-per-level"));
        if (Mod::get()->getSettingValue<bool>("rotate-levels") && g_deathsOnLevel >= limit) {
            g_switchPending = true;
        }
    }
};
