#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
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

static constexpr int K = 16;       // columnas que ve hacia adelante
static constexpr int NFLAG = 17;   // tipos de objetos especiales

// 0-3 pads (amarillo, rosa, rojo, azul), 4-10 orbes (amarillo, rosa, rojo, azul, verde, negro, dash),
// 11 portal de modo, 12 gravedad, 13 tamano, 14 velocidad, 15 dual/espejo/teletransporte,
// 16 orbe personalizado
static int specialIndex(GameObject* o) {
    switch (o->m_objectType) {
        case GameObjectType::YellowJumpPad: return 0;
        case GameObjectType::PinkJumpPad: return 1;
        case GameObjectType::RedJumpPad: return 2;
        case GameObjectType::GravityPad: return 3;
        case GameObjectType::YellowJumpRing: return 4;
        case GameObjectType::PinkJumpRing: return 5;
        case GameObjectType::RedJumpRing: return 6;
        case GameObjectType::GravityRing: return 7;
        case GameObjectType::GreenRing: return 8;
        case GameObjectType::DropRing: return 9;
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing: return 10;
        case GameObjectType::CubePortal:
        case GameObjectType::ShipPortal:
        case GameObjectType::BallPortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::SpiderPortal: return 11;
        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal: return 12;
        case GameObjectType::MiniSizePortal:
        case GameObjectType::RegularSizePortal: return 13;
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::InverseMirrorPortal:
        case GameObjectType::NormalMirrorPortal:
        case GameObjectType::TeleportPortal: return 15;
        case GameObjectType::CustomRing: return 16;
        default: break;
    }
    switch (o->m_objectID) {
        case 200: case 201: case 202: case 203: case 1334: return 14;
        default: return -1;
    }
}

static int modeOf(PlayerObject* pl) {
    if (pl->m_isShip) return 1;
    if (pl->m_isBall) return 2;
    if (pl->m_isBird) return 3;
    if (pl->m_isDart) return 4;
    if (pl->m_isRobot) return 5;
    if (pl->m_isSpider) return 6;
    return 0;
}

// Hitbox real de un peligro: circulo para sierras, rectangulo de colision para el resto
static CCRect hazardRect(GameObject* o) {
    float r = o->m_objectRadius;
    if (r > 0.f) {
        return CCRect(o->getPositionX() - r, o->getPositionY() - r, r * 2.f, r * 2.f);
    }
    return o->getObjectRect();
}

// Lo que ve de cada columna desde la altura de un jugador
struct Col {
    float below = 90.f;
    float above = 0.f;
    float hazRel = 0.f;
    float hazBot = 0.f;
    float hazTop = 0.f;
    float bestHaz = 1e9f;
    int block = 0;
    int haz = 0;
};

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

// ---------- Grabacion de tu gameplay ----------
static bool g_userHolding = false;
static std::ofstream g_rec;

static void recWrite(const std::string& line) {
    if (!g_rec.is_open()) {
        auto path = Mod::get()->getSaveDir() / "gameplay.csv";
        g_rec.open(path, std::ios::app);
        log::info("Grabando gameplay en {}", path.string());
    }
    g_rec << line << "\n";
    if (line.rfind("END", 0) == 0) g_rec.flush();
}

// Detecta cuando TU mantienes o sueltas el clic
class $modify(RecGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (isPlayer1 && button == 1) g_userHolding = down;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

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

        // objetos del nivel (se leen sus posiciones ACTUALES en cada frame)
        std::vector<GameObject*> rawHazards;
        std::vector<GameObject*> rawSolids;
        std::vector<GameObject*> rawSlopes;
        std::vector<std::pair<GameObject*, int>> rawSpecials;

        std::vector<float> clusterX;     // inicio de cada grupo de pinchos
        bool eyesReady = false;

        int recRows = 0;
        bool recDead = false;
    };

    void addObject(GameObject* obj) {
        PlayLayer::addObject(obj);
        if (!obj) return;
        int si = specialIndex(obj);
        if (si >= 0) {
            m_fields->rawSpecials.push_back({obj, si});
        } else if (obj->m_objectType == GameObjectType::Hazard) {
            m_fields->rawHazards.push_back(obj);
        } else if (obj->m_objectType == GameObjectType::Slope) {
            m_fields->rawSlopes.push_back(obj);
        } else if (obj->m_objectType == GameObjectType::Solid) {
            m_fields->rawSolids.push_back(obj);
        }
    }

    void prepareEyes() {
        // grupos de pinchos segun su posicion inicial (para la ayuda de recompensa)
        std::vector<int> hc;
        for (auto o : m_fields->rawHazards) {
            hc.push_back(static_cast<int>(o->getPositionX() / 30.f));
        }
        std::sort(hc.begin(), hc.end());
        hc.erase(std::unique(hc.begin(), hc.end()), hc.end());
        m_fields->clusterX.clear();
        int prev = -10;
        for (int c : hc) {
            if (c != prev + 1) m_fields->clusterX.push_back(c * 30.f);
            prev = c;
        }
        m_fields->eyesReady = true;
        log::info("Ojos listos: {} peligros, {} bloques, {} rampas, {} especiales",
                  m_fields->rawHazards.size(), m_fields->rawSolids.size(),
                  m_fields->rawSlopes.size(), m_fields->rawSpecials.size());
    }

    // Suelo, techo, muros y peligros de cada columna, vistos desde la altura refY
    void scanCols(float refY, float px, Col* out) {
        for (int k = 0; k < K; k++) {
            out[k] = Col();
            out[k].above = refY + 999.f;
        }
        for (auto o : m_fields->rawSolids) {
            if (o->m_isPassable) continue;
            float ox = o->getPositionX();
            if (ox < px - 100.f || ox > px + 620.f) continue;
            CCRect r = o->getObjectRect();
            int a = std::max(static_cast<int>(std::floor((r.getMinX() + 1.f - px) / 30.f)), 0);
            int b = std::min(static_cast<int>(std::floor((r.getMaxX() - 1.f - px) / 30.f)), K - 1);
            float bot = r.getMinY();
            float top = r.getMaxY();
            for (int k = a; k <= b; k++) {
                if (bot < refY + 12.f && top > refY - 12.f) {
                    out[k].block = 1; out[k].below = std::max(out[k].below, top);
                } else if (top <= refY - 12.f) {
                    out[k].below = std::max(out[k].below, top);
                } else {
                    out[k].above = std::min(out[k].above, bot);
                }
            }
        }
        for (auto o : m_fields->rawHazards) {
            float ox = o->getPositionX();
            if (ox < px - 100.f || ox > px + 620.f) continue;
            CCRect r = hazardRect(o);
            int a = std::max(static_cast<int>(std::floor((r.getMinX() + 1.f - px) / 30.f)), 0);
            int b = std::min(static_cast<int>(std::floor((r.getMaxX() - 1.f - px) / 30.f)), K - 1);
            if (r.getMaxX() < px - 15.f) continue;
            if (b < a) b = a;
            if (a >= K) continue;
            float bot = r.getMinY();
            float top = r.getMaxY();
            float dv = 0.f;
            float rel = 0.f;
            if (refY < bot) {
                dv = bot - refY;
                rel = dv / 60.f;
            } else if (refY > top) {
                dv = refY - top;
                rel = -dv / 60.f;
            }
            for (int k = a; k <= b; k++) {
                out[k].haz = 1;
                if (dv < out[k].bestHaz) {
                    out[k].bestHaz = dv;
                    out[k].hazRel = clampv((r.getMinX() - px - 30.f * k) / 30.f, -20.f, 20.f);
                    out[k].hazBot = clampv((bot - refY) / 60.f, -3.f, 3.f);
                    out[k].hazTop = clampv((top - refY) / 60.f, -3.f, 3.f);
                }
            }
        }
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
        int mode = modeOf(m_player1);

        // segundo jugador (modo dual)
        bool dual = m_player2 && m_gameState.m_isDualMode;
        float y2 = 0.f;
        float yv2 = 0.f;
        int ground2 = 0;
        int upside2 = 0;
        int mode2 = 0;
        if (dual) {
            y2 = m_player2->getPositionY();
            yv2 = static_cast<float>(m_player2->m_yVelocity);
            ground2 = m_player2->m_isOnGround ? 1 : 0;
            upside2 = m_player2->m_isUpsideDown ? 1 : 0;
            mode2 = modeOf(m_player2);
        }

        int levelId = static_cast<int>(m_level->m_levelID.value());
        int passed = static_cast<int>(
            std::lower_bound(m_fields->clusterX.begin(), m_fields->clusterX.end(), px) -
            m_fields->clusterX.begin());
        float portalY = static_cast<float>(m_gameState.m_portalY);

        std::string s = fmt::format(
            "{},{:.4f},{},{},{:.2f},{:.3f},{},{},{},{:.2f},{},{},{:.2f},{:.3f},{},{},{},{:.1f}",
            status, p, levelId, passed, y, yv, ground, upside, mini, speed, mode,
            dual ? 1 : 0, y2, yv2, ground2, upside2, mode2, portalY);

        Col c1[K];
        Col c2[K];
        this->scanCols(y, px, c1);
        if (dual) this->scanCols(y2, px, c2);

        // rampas, objetos especiales y teletransportes (siempre desde el jugador 1)
        struct Slp {
            int has = 0;
            float surf = 0.f;
            int floorTop = 0;
            int uphill = 0;
            int hazard = 0;
            float best = 1e9f;
        };
        Slp slp[K];
        int flags[K][NFLAG];
        float specRel[K];
        float bestSpec[K];
        float tele[K];
        for (int k = 0; k < K; k++) {
            specRel[k] = 0.f;
            bestSpec[k] = 1e9f;
            tele[k] = 0.f;
            for (int j = 0; j < NFLAG; j++) flags[k][j] = 0;
        }

        for (auto o : m_fields->rawSlopes) {
            float ox = o->getPositionX();
            if (ox < px - 100.f || ox > px + 620.f) continue;
            CCRect r = o->getObjectRect();
            int a = std::max(static_cast<int>(std::floor((r.getMinX() + 1.f - px) / 30.f)), 0);
            int b = std::min(static_cast<int>(std::floor((r.getMaxX() - 1.f - px) / 30.f)), K - 1);
            for (int k = a; k <= b; k++) {
                float xc = clampv(px + (static_cast<float>(k) + 0.5f) * 30.f, r.getMinX(), r.getMaxX());
                float sy = static_cast<float>(o->slopeYPos(xc));
                float d = std::fabs(sy - y);
                if (d < slp[k].best) {
                    slp[k].best = d;
                    slp[k].has = 1;
                    slp[k].surf = clampv((sy - y) / 60.f, -3.f, 3.f);
                    slp[k].floorTop = o->slopeFloorTop() ? 1 : 0;
                    slp[k].uphill = o->m_slopeUphill ? 1 : 0;
                    slp[k].hazard = o->m_slopeIsHazard ? 1 : 0;
                }
            }
        }
        for (auto& pr : m_fields->rawSpecials) {
            float ox = pr.first->getPositionX();
            int k = static_cast<int>(std::floor((ox - px) / 30.f));
            if (k < 0 || k >= K) continue;
            float oy = pr.first->getPositionY();
            flags[k][pr.second] = 1;
            float d = std::fabs(oy - y);
            if (d < bestSpec[k]) {
                bestSpec[k] = d;
                specRel[k] = clampv((oy - y) / 60.f, -3.f, 3.f);
            }
            if (pr.first->m_objectType == GameObjectType::TeleportPortal) {
                auto tp = static_cast<TeleportPortalObject*>(pr.first);
                if (tp->m_orangePortal) {
                    tele[k] = clampv((tp->m_orangePortal->getPositionY() - y) / 60.f, -3.f, 3.f);
                }
            }
        }

        for (int k = 0; k < K; k++) {
            // jugador 1: suelo, techo, muro, peligro (+ su extension vertical)
            float relBelow = clampv((c1[k].below - y) / 60.f, -3.f, 0.f);
            float relAbove = clampv((c1[k].above - y) / 60.f, 0.f, 3.f);
            s += fmt::format(",{:.2f},{:.2f},{},{},{:.2f},{:.2f},{:.2f}", relBelow, relAbove,
                             c1[k].block, c1[k].haz, c1[k].hazRel, c1[k].hazBot, c1[k].hazTop);
            // rampa
            s += fmt::format(",{},{:.2f},{},{},{}", slp[k].has, slp[k].surf,
                             slp[k].floorTop, slp[k].uphill, slp[k].hazard);
            // pads, orbes, portales
            for (int j = 0; j < NFLAG; j++) s += fmt::format(",{}", flags[k][j]);
            s += fmt::format(",{:.2f},{:.2f}", specRel[k], tele[k]);
            // jugador 2 (ceros si no hay dual)
            if (dual) {
                float rb2 = clampv((c2[k].below - y2) / 60.f, -3.f, 0.f);
                float ra2 = clampv((c2[k].above - y2) / 60.f, 0.f, 3.f);
                s += fmt::format(",{:.2f},{:.2f},{},{},{:.2f}", rb2, ra2,
                                 c2[k].block, c2[k].haz, c2[k].hazRel);
            } else {
                s += ",0.00,0.00,0,0,0.00";
            }
        }
        return s;
    }

    bool anyDead() {
        return m_player1->m_isDead || (m_player2 && m_player2->m_isDead);
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
        g_userHolding = false;
        if (m_fields->recRows > 0 && !m_fields->recDead) recWrite("END,2");
        m_fields->recRows = 0;
        m_fields->recDead = false;
    }

    void recordFrame() {
        if (!m_fields->eyesReady) this->prepareEyes();
        if (this->anyDead()) {
            if (!m_fields->recDead && m_fields->recRows > 0) recWrite("END,0");
            m_fields->recDead = true;
            return;
        }
        m_fields->recDead = false;
        recWrite(fmt::format("R,{},{}", g_userHolding ? 1 : 0, this->makeState(0)));
        m_fields->recRows++;
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

        // --- Grabar tu gameplay (solo con el bot apagado) ---
        if (!botOn && Mod::get()->getSettingValue<bool>("record-mode")) {
            this->recordFrame();
        }

        if (!botOn) return;
        if (!m_fields->eyesReady) this->prepareEyes();

        // --- Conectar con Python (reintenta cada 2 segundos) ---
        if (g_sock == INVALID_SOCKET) {
            g_retryTimer -= dt;
            if (g_retryTimer > 0.f) return;
            g_retryTimer = 2.f;
            if (!connectToPython()) return;
        }

        bool dead = this->anyDead();
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
        if (!botOn) {
            if (Mod::get()->getSettingValue<bool>("record-mode") && m_fields->recRows > 0) {
                recWrite("END,1");
                m_fields->recRows = 0;
                m_fields->recDead = true;
            }
            PlayLayer::levelComplete();
            return;
        }
        if (!m_player1 || g_sock == INVALID_SOCKET || !m_fields->eyesReady) {
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
