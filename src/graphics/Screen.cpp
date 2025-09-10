/*
... (license block unchanged) ...
*/
#include "Screen.h"
#include "NodeDB.h"
#include "PowerMon.h"
#include "Throttle.h"
#include "configuration.h"
#if HAS_SCREEN
#include <OLEDDisplay.h>

#include "DisplayFormatters.h"
#include "TimeFormatters.h"
#include "draw/ClockRenderer.h"
#include "draw/DebugRenderer.h"
#include "draw/MenuHandler.h"
#include "draw/MessageRenderer.h"
#include "draw/NodeListRenderer.h"
#include "draw/NotificationRenderer.h"
#include "draw/UIRenderer.h"
#include "modules/CannedMessageModule.h"

#if !MESHTASTIC_EXCLUDE_GPS
#include "GPS.h"
#include "buzz.h"
#endif
#include "FSCommon.h"
#include "MeshService.h"
#include "RadioLibInterface.h"
#include "error.h"
#include "gps/GeoCoord.h"
#include "gps/RTC.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/emotes.h"
#include "graphics/images.h"
#include "input/TouchScreenImpl1.h"
#include "main.h"
#include "mesh-pb-constants.h"
#include "mesh/Channels.h"
#include "mesh/generated/meshtastic/deviceonly.pb.h"
#include "meshUtils.h"
#include "modules/ExternalNotificationModule.h"
#include "modules/TextMessageModule.h"
#include "modules/WaypointModule.h"
#include "modules/ChatHistoryStore.h"
#include "sleep.h"
#include "target_specific.h"
#include "mesh/MeshTypes.h"   // para NODENUM_BROADCAST
#include "detect/ScanI2C.h"

#include <set>
#include <map>
#include <vector>
#include <algorithm>

using graphics::Emote;
using graphics::emotes;
using graphics::numEmotes;

extern uint16_t TFT_MESH;
extern bool g_chatScrollByPress;  // viene de MenuHandler.cpp
extern bool kb_found;

#if HAS_WIFI && !defined(ARCH_PORTDUINO)
#include "mesh/wifi/WiFiAPClient.h"
#endif

#ifdef ARCH_ESP32
#endif

#if ARCH_PORTDUINO
#include "modules/StoreForwardModule.h"
#include "platform/portduino/PortduinoGlue.h"
#endif

using namespace meshtastic; /** @todo remove */

namespace graphics
{
// Compat: el nombre/alcance del enum de ScanI2C cambia entre forks.
// Mapeamos por valor: 0 -> I2C_ONE, !=0 -> I2C_TWO
template <typename PortT>
static inline HW_I2C hwPortFrom(PortT p) {
    return (static_cast<int>(p) == 0) ? HW_I2C::I2C_ONE : HW_I2C::I2C_TWO;
}
	
	
	
// --- volver al mismo chat tras enviar texto ---
static int  s_returnToFrame   = -1;
static bool s_reFocusAfterSend = false;


// Semilla para abrir pestañas de chat de canal en el arranque
static bool s_seededChannelTabs = false;
static void seedChannelTabsFromConfig();

// --- Helpers para filtrar opciones según CardKB ---
static uint8_t filterByCardKB(const char* const *srcOptions, const int *srcEnums, uint8_t srcCount,
                              const char** dstOptions, int* dstEnums)
{
    // Regla:
    //  - Con CardKB (kb_found=true): ocultar "New preset msg"
    //  - Sin CardKB (kb_found=false): ocultar "New text msg"
    uint8_t n = 0;
    for (uint8_t i = 0; i < srcCount; ++i) {
        const char* s = srcOptions[i];
        if (!s) continue;

        bool isPreset = (strncmp(s, "New preset msg", 14) == 0);
        bool isText   = (strncmp(s, "New text msg", 12)   == 0);

        if (kb_found) {
            if (isPreset) continue; // ocultar "New preset msg"
        } else {
            if (isText) continue;   // ocultar "New text msg"
        }

        dstOptions[n] = s;
        if (srcEnums) dstEnums[n] = srcEnums[i];
        ++n;
    }
    return n;
}

static void showMenuFilteredByCardKB(const char* title,
                                     const char* const *options, const int *enums, uint8_t count,
                                     std::function<void(int)> cb)
{
    // Buffers estáticos para asegurar vida útil hasta que el usuario seleccione
    static const char* filteredOpts[20];
    static int         filteredEnums[20];

    uint8_t filteredCount = filterByCardKB(options, enums, count, filteredOpts, filteredEnums);

    // Si no queda ninguna opción, salimos sin mostrar nada
    if (filteredCount == 0) {
        return;
    }

    // Montaje del banner de selección
    NotificationRenderer::resetBanner();
    strlcpy(NotificationRenderer::alertBannerMessage, title, sizeof(NotificationRenderer::alertBannerMessage));
    NotificationRenderer::curSelected           = 0;
    NotificationRenderer::alertBannerOptions    = filteredCount;
    NotificationRenderer::optionsArrayPtr       = filteredOpts;
    NotificationRenderer::optionsEnumPtr        = (enums ? filteredEnums : nullptr);
    NotificationRenderer::alertBannerCallback   = cb;
    NotificationRenderer::alertBannerUntil      = 0;
    NotificationRenderer::current_notification_type = notificationTypeEnum::selection_picker;
}

// === Helpers para mostrar edad de último mensaje con s/m/h/D ===
static String ageLabel(uint32_t tsSec)
{
    uint32_t nowSec = (uint32_t)time(nullptr);
    if (nowSec == 0) {
        nowSec = millis() / 1000;
    }

    uint32_t diff   = (nowSec > tsSec) ? (nowSec - tsSec) : 0;

    if (diff < 60) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%lus", (unsigned long)diff);
        return String(buf);
    } else if (diff < 3600) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%lum", (unsigned long)(diff / 60));
        return String(buf);
    } else if (diff < 86400) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%luh", (unsigned long)(diff / 3600));
        return String(buf);
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%luD", (unsigned long)(diff / 86400));
        return String(buf);
    }
}

static String currentChatAgeLabel(uint32_t nodeIdOrDest, uint8_t ch)
{
    uint32_t ts = 0;
    bool ok = false;

    if (nodeIdOrDest == NODENUM_BROADCAST) {
        const auto& v = chat::ChatHistoryStore::instance().getCHAN(ch);
        if (!v.empty()) { ts = v.back().ts; ok = true; }
    } else {
        const auto& v = chat::ChatHistoryStore::instance().getDM(nodeIdOrDest);
        if (!v.empty()) { ts = v.back().ts; ok = true; }
    }

    return ok ? ageLabel(ts) : String("");
}

// ====== Header pendiente para el teclado (fix "To:" en canales) ======
static std::string g_pendingKeyboardHeader;

// === Chat tabs: state & draw helpers ===
static std::vector<uint32_t> g_favChatNodes;
static size_t g_favChatFirst = (size_t)-1;
static size_t g_favChatLast  = (size_t)-1;

static std::vector<uint8_t>  g_chanTabs;
static size_t g_chanTabFirst = (size_t)-1;
static size_t g_chanTabLast  = (size_t)-1;

// “Favoritos” de canal gestionados solo desde Screen.cpp
static std::set<uint8_t> g_favChannelTabs;

static void seedChannelTabsFromConfig()
{
    if (s_seededChannelTabs) return;
    s_seededChannelTabs = true;
    int n = channels.getNumChannels();
    for (int i = 0; i < n; ++i) {
        const meshtastic_Channel c = channels.getByIndex(i);
        bool present = (i == 0);
        if (c.settings.name[0]) present = true;
        if (present) g_favChannelTabs.insert((uint8_t)i);
    }
}


// ===== Scroll horizontal solo en línea seleccionada =====
static bool g_chatScrollActive = false; // true si algún frame pintó marquee este ciclo

struct ScrollState {
    int sel = 0;            // línea seleccionada (0..visible-1)
    int offset = 0;         // desplazamiento horizontal (caracteres)
    uint32_t lastMs = 0;    // última actualización
};

static std::map<uint32_t, ScrollState> g_nodeScroll; // por nodo (DM)
static std::map<uint8_t , ScrollState> g_chanScroll; // por canal

// Helpers (por si algún día tratamos canal como “nodo virtual”)
static inline bool isVirtualChannelNode(uint32_t nodeId) { return (nodeId & 0xC0000000u) == 0xC0000000u; }
static inline uint8_t channelOfVirtual(uint32_t nodeId)  { return (uint8_t)(nodeId & 0xFFu); }
static inline uint32_t makeVirtualChannelNode(uint8_t ch) { return 0xC0000000u | ch; }

// Marquee helper: devuelve ventana de 'cap' chars, avanzando cada ~200ms
static std::string marqueeSlice(const std::string& in, ScrollState& st, int cap, bool advance)
{
    if ((int)in.size() <= cap) { st.offset = 0; return in; }

    const uint32_t stepMs = 200;
    const std::string sep = "   ";
    if (advance) {
        uint32_t now = millis();
        if (now - st.lastMs >= stepMs) {
            st.lastMs = now;
            st.offset = st.offset + 1;
        }
    }

    std::string padded = in + sep;
    int n = (int)padded.size();
    int o = (n > 0) ? (st.offset % n) : 0;

    if (o + cap <= n) return padded.substr(o, cap);
    std::string s1 = padded.substr(o);
    return s1 + padded.substr(0, cap - (int)s1.size());
}

// ===================== NODO =====================
static void openChatActionsForNode(uint32_t nodeId)
{
    // Opciones dinámicas (máx 5 visibles aquí)
    enum { kPreset = 1, kFree = 2, kRemove = 3, kInfo = 4, kScroll = 5, kBack = 6 };

    static const char* opts[6];
    static int         enums[6];
    int count = 0;

    // Preset / Freetext según CardKB
    if (kb_found) {
        opts[count]  = "New Freetext Msg";
        enums[count] = kFree;
        count++;
    } else {
        opts[count]  = "New Preset Msg";
        enums[count] = kPreset;
        count++;
    }

    // Comunes
    opts[count]  = "Remove Chat";
    enums[count] = kRemove;
    count++;

    opts[count]  = "Node Info";
    enums[count] = kInfo;
    count++;

    // Scroll Btn solo si NO hay CardKB
    static char scrollLabel[24];
    if (!kb_found) {
        snprintf(scrollLabel, sizeof(scrollLabel), "Scroll Btn: %s", g_chatScrollByPress ? "ON" : "OFF");
        opts[count]  = scrollLabel;
        enums[count] = kScroll;
        count++;
    }

    opts[count]  = "Back";
    enums[count] = kBack;
    count++;

    BannerOverlayOptions o;
    o.message         = "Menu Chat";
    o.durationMs      = 0;
    o.optionsArrayPtr = opts;
    o.optionsEnumPtr  = enums;
    o.optionsCount    = count;

    o.bannerCallback  = [nodeId](int sel) {
        // Cerrar el banner antes de cambiar pantallas/estados
        NotificationRenderer::pauseBanner      = true;
        NotificationRenderer::alertBannerUntil = 1;

        switch (sel) {
        case kPreset:
            if (cannedMessageModule) cannedMessageModule->LaunchWithDestination(nodeId);
            break;

        case kFree:
            if (cannedMessageModule) cannedMessageModule->LaunchFreetextWithDestination(nodeId);
            break;

        case kRemove:
            if (nodeDB) nodeDB->set_favorite(false, nodeId);
            if (screen) screen->setFrames(Screen::FOCUS_PRESERVE);
            break;

        case kInfo:
            if (screen) {
                graphics::UIRenderer::currentFavoriteNodeNum = nodeId;
                screen->openNodeInfoFor(nodeId);
            }
            break;

        case kScroll:
            g_chatScrollByPress = !g_chatScrollByPress;
            if (screen) screen->showSimpleBanner(g_chatScrollByPress ? "Scroll Btn: ON" : "Scroll Btn: OFF", 1200);
            break;

        default:
            break;
        }

        if (screen) screen->forceDisplay(true);
    };

    screen->showOverlayBanner(o);
}


// ===================== CANAL =====================
static void openChatActionsForChannel(uint8_t ch)
{
    enum { kPreset = 1, kFree = 2, kRemove = 3, kScroll = 4, kBack = 5 };

    static const char* opts[5];
    static int         enums[5];
    int count = 0;

    // Preset / Freetext según CardKB
    if (kb_found) {
        opts[count]  = "New Freetext Msg";
        enums[count] = kFree;
        count++;
    } else {
        opts[count]  = "New Preset Msg";
        enums[count] = kPreset;
        count++;
    }

    // Comunes
    opts[count]  = "Remove Chat";
    enums[count] = kRemove;
    count++;

    // Scroll Btn solo si NO hay CardKB
    static char scrollLabel[24];
    if (!kb_found) {
        snprintf(scrollLabel, sizeof(scrollLabel), "Scroll Btn: %s", g_chatScrollByPress ? "ON" : "OFF");
        opts[count]  = scrollLabel;
        enums[count] = kScroll;
        count++;
    }

    opts[count]  = "Back";
    enums[count] = kBack;
    count++;

    // Título con nombre del canal (si existe)
    const meshtastic_Channel c = channels.getByIndex(ch);
    const char *cname = (c.settings.name[0]) ? c.settings.name : nullptr;
    char title[64];
    if (cname) snprintf(title, sizeof(title), "Canal: %s", cname);
    else       snprintf(title, sizeof(title), "Canal %u", (unsigned)ch);

    BannerOverlayOptions o;
    o.message         = title;
    o.durationMs      = 0;
    o.optionsArrayPtr = opts;
    o.optionsEnumPtr  = enums;
    o.optionsCount    = count;

    o.bannerCallback  = [ch](int sel) {
        // Cerrar banner antes de actuar (evita estados raros)
        NotificationRenderer::pauseBanner      = true;
        NotificationRenderer::alertBannerUntil = 1;

        // Preparar header del teclado (si luego abres input)
        const meshtastic_Channel cc = channels.getByIndex(ch);
        const char *cname2 = (cc.settings.name[0]) ? cc.settings.name : nullptr;
        char hdr[64];
        if (cname2) snprintf(hdr, sizeof(hdr), "To: %s", cname2);
        else        snprintf(hdr, sizeof(hdr), "To: Channel %u", (unsigned)ch);
        g_pendingKeyboardHeader = hdr;

        // Asegura canal activo y marcado como favorito-tab
        channels.setActiveByIndex(ch);
        g_favChannelTabs.insert(ch);

        switch (sel) {
        case kPreset:
            if (cannedMessageModule) cannedMessageModule->LaunchWithDestination(NODENUM_BROADCAST, ch);
            break;
        case kFree:
            if (cannedMessageModule) cannedMessageModule->LaunchFreetextWithDestination(NODENUM_BROADCAST, ch);
            break;
        case kRemove:
            g_favChannelTabs.erase(ch);
            if (screen) screen->setFrames(Screen::FOCUS_PRESERVE);
            break;
        case kScroll:
            g_chatScrollByPress = !g_chatScrollByPress;
            if (screen) screen->showSimpleBanner(g_chatScrollByPress ? "Scroll Btn: ON" : "Scroll Btn: OFF", 1200);
            break;
        default:
            break;
        }

        if (screen) screen->forceDisplay(true);
    };

    screen->showOverlayBanner(o);
}






// Small text line helper
static void drawLineSmall(OLEDDisplay *display, int16_t x, int16_t y, const char* s) {
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    display->drawString(x, y, s);
}



static void drawFavNodeChatFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    if (g_favChatFirst == (size_t)-1 || g_favChatLast == (size_t)-1) return;
    uint8_t cf = state->currentFrame;
    size_t idx = (size_t)cf - g_favChatFirst;
    if (idx >= g_favChatNodes.size()) return;

    uint32_t nodeId = g_favChatNodes[idx];
    using chat::ChatHistoryStore;
    auto &store = ChatHistoryStore::instance();
    const auto &q = store.getDM(nodeId);

    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeId);
    const char* alias = (node && node->has_user && node->user.long_name[0]) ? node->user.long_name : nullptr;

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    // === Tiempo dinámico según mensaje seleccionado ===
    ScrollState &st = g_nodeScroll[nodeId];
    uint32_t tsSel = 0;
    if (!q.empty()) {
        int i = (int)q.size() - 1 - st.sel;
        if (i >= 0 && i < (int)q.size()) {
            tsSel = q[i].ts;
        }
    }
    String age = (tsSel > 0) ? ageLabel(tsSel) : String("");

    char title[64];
    if (alias)  std::snprintf(title, sizeof(title), "%s (%s)", alias, age.c_str());
    else        std::snprintf(title, sizeof(title), "%08X (%s)", (unsigned)nodeId, age.c_str());
    display->drawString(x, y, title);

    const int lineH = 10;
    const int top   = y + 16;
    const int h     = display->height();
    const int maxLines = (h - 16) / lineH > 4 ? (h - 16) / lineH : 4;

    display->setFont(FONT_SMALL);

    const int visible = std::min((int)q.size(), maxLines);
    if (visible <= 0) {
        drawLineSmall(display, x, top, "Waiting");
        return;
    }

    if (st.sel < 0) st.sel = 0;
    if (st.sel >= visible) st.sel = visible - 1;

    for (int l = 0; l < visible; ++l) {
        int i = (int)q.size() - 1 - l;
        const auto &e = q[i];

        // Etiqueta fija
        std::string who = e.outgoing ? "Send" : "Received";

        // Texto completo
        std::string base = who + ": " + e.text;
        std::string view;
        bool needScroll = false;
        const int cap = 28;

        if (l == st.sel) {
            view = marqueeSlice(base, st, cap, /*advance*/ true);
            needScroll = ((int)base.size() > cap);
        } else {
            if ((int)base.size() > cap) view = base.substr(0, cap - 3) + "...";
            else view = base;
        }
        if (needScroll) g_chatScrollActive = true;

        int lineY = top + l * lineH;
        if (l == st.sel) {
            display->fillRect(x, lineY, display->getWidth(), lineH);
            display->setColor(BLACK);
            drawLineSmall(display, x, lineY, view.c_str());
            display->setColor(WHITE);
        } else {
            drawLineSmall(display, x, lineY, view.c_str());
        }
    }
}

static void drawChannelChatTabFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    if (g_chanTabFirst == (size_t)-1 || g_chanTabLast == (size_t)-1) return;
    uint8_t cf = state->currentFrame;
    size_t idx = (size_t)cf - g_chanTabFirst;
    if (idx >= g_chanTabs.size()) return;

    uint8_t ch = g_chanTabs[idx];
    using chat::ChatHistoryStore;
    auto &store = ChatHistoryStore::instance();
    const auto &q = store.getCHAN(ch);

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    const meshtastic_Channel c = channels.getByIndex(ch);
    const char *cname = (c.settings.name[0]) ? c.settings.name : nullptr;

    // === Tiempo dinámico según mensaje seleccionado ===
    ScrollState &st = g_chanScroll[ch];
    uint32_t tsSel = 0;
    if (!q.empty()) {
        int i = (int)q.size() - 1 - st.sel;
        if (i >= 0 && i < (int)q.size()) {
            tsSel = q[i].ts;
        }
    }
    String age = (tsSel > 0) ? ageLabel(tsSel) : String("");

    char title[64];
    if (cname) std::snprintf(title, sizeof(title), "@%s (%s)", cname, age.c_str());
    else       std::snprintf(title, sizeof(title), "@Channel %u (%s)", (unsigned)ch, age.c_str());
    display->drawString(x, y, title);

    const int lineH = 10;
    const int top   = y + 16;
    const int h     = display->height();
    const int maxLines = (h - 16) / lineH > 4 ? (h - 16) / lineH : 4;

    display->setFont(FONT_SMALL);

    const int visible = std::min((int)q.size(), maxLines);
    if (visible <= 0) {
        drawLineSmall(display, x, top, "Waiting...");
        return;
    }

    if (st.sel < 0) st.sel = 0;
    if (st.sel >= visible) st.sel = visible - 1;

    for (int l = 0; l < visible; ++l) {
        int i = (int)q.size() - 1 - l;
        const auto &e = q[i];

        std::string who;
        if (e.outgoing) who = "Send";
        else {
            const meshtastic_NodeInfoLite *sender = (e.node) ? nodeDB->getMeshNode(e.node) : nullptr;
            if (sender && sender->has_user && sender->user.long_name[0]) who = sender->user.long_name;
            else if (e.node) { char buf[9]; std::snprintf(buf, sizeof(buf), "%08X", (unsigned)e.node); who = buf; }
            else who = "??";
        }

        std::string base = who + ": " + e.text;
        std::string view;
        bool needScroll = false;
        const int cap = 28;

        if (l == st.sel) {
            view = marqueeSlice(base, st, cap, /*advance*/ true);
            needScroll = ((int)base.size() > cap);
        } else {
            if ((int)base.size() > cap) view = base.substr(0, cap - 3) + "...";
            else view = base;
        }
        if (needScroll) g_chatScrollActive = true;

        int lineY = top + l * lineH;
        if (l == st.sel) {
            display->fillRect(x, lineY, display->getWidth(), lineH);
            display->setColor(BLACK);
            drawLineSmall(display, x, lineY, view.c_str());
            display->setColor(WHITE);
        } else {
            drawLineSmall(display, x, lineY, view.c_str());
        }
    }
}

// Visible area
#define IDLE_FRAMERATE 1 // fps

// DEBUG
#define NUM_EXTRA_FRAMES 3 // text message and debug frame

FrameCallback *normalFrames;
static uint32_t targetFramerate = IDLE_FRAMERATE;

uint32_t logo_timeout = 5000;
uint32_t dopThresholds[5] = {2000, 1000, 500, 200, 100};

std::vector<MeshModule *> moduleFrames;

std::vector<std::string> functionSymbol;
std::string functionSymbolString;

#if HAS_GPS
GeoCoord geoCoord;
#endif

#ifdef SHOW_REDRAWS
static bool heartbeat = false;
#endif

#include "graphics/ScreenFonts.h"
#include <Throttle.h>

extern bool hasUnreadMessage;

// ==============================
// Overlay Alert Banner Renderer
// ==============================

void Screen::openNodeInfoFor(NodeNum nodeNum)
{
    // Guardamos qué nodo se debe mostrar
    graphics::UIRenderer::currentFavoriteNodeNum = nodeNum;

    // Creamos un FrameCallback con la función drawNodeInfoDirect
    setFrameImmediateDraw(new FrameCallback(
        [](OLEDDisplay *d, OLEDDisplayUiState *s, int16_t x, int16_t y) {
            graphics::UIRenderer::drawNodeInfoDirect(d, s, x, y);
        }
    ));
}

void Screen::showSimpleBanner(const char *message, uint32_t durationMs)
{
    BannerOverlayOptions options;
    options.message = message;
    options.durationMs = durationMs;
    options.notificationType = notificationTypeEnum::text_banner;
    showOverlayBanner(options);
}

void Screen::showOverlayBanner(BannerOverlayOptions banner_overlay_options)
{
#ifdef USE_EINK
    EINK_ADD_FRAMEFLAG(dispdev, DEMAND_FAST);
#endif
    strncpy(NotificationRenderer::alertBannerMessage, banner_overlay_options.message, 255);
    NotificationRenderer::alertBannerMessage[255] = '\0';
    NotificationRenderer::alertBannerUntil =
        (banner_overlay_options.durationMs == 0) ? 0 : millis() + banner_overlay_options.durationMs;
    NotificationRenderer::optionsArrayPtr = banner_overlay_options.optionsArrayPtr;
    NotificationRenderer::optionsEnumPtr = banner_overlay_options.optionsEnumPtr;
    NotificationRenderer::alertBannerOptions = banner_overlay_options.optionsCount;
    NotificationRenderer::alertBannerCallback = banner_overlay_options.bannerCallback;
    NotificationRenderer::curSelected = banner_overlay_options.InitialSelected;
    NotificationRenderer::pauseBanner = false;
    NotificationRenderer::current_notification_type = notificationTypeEnum::selection_picker;
    static OverlayCallback overlays[] = {graphics::UIRenderer::drawNavigationBar, NotificationRenderer::drawBannercallback};
    ui->setOverlays(overlays, sizeof(overlays) / sizeof(overlays[0]));
    ui->setTargetFPS(60);
    ui->update();
}

void Screen::showNodePicker(const char *message, uint32_t durationMs, std::function<void(uint32_t)> bannerCallback)
{
#ifdef USE_EINK
    EINK_ADD_FRAMEFLAG(dispdev, DEMAND_FAST);
#endif
    nodeDB->pause_sort(true);
    strncpy(NotificationRenderer::alertBannerMessage, message, 255);
    NotificationRenderer::alertBannerMessage[255] = '\0';
    NotificationRenderer::alertBannerUntil = (durationMs == 0) ? 0 : millis() + durationMs;
    NotificationRenderer::alertBannerCallback = bannerCallback;
    NotificationRenderer::pauseBanner = false;
    NotificationRenderer::curSelected = 0;
    NotificationRenderer::current_notification_type = notificationTypeEnum::node_picker;

    static OverlayCallback overlays[] = {graphics::UIRenderer::drawNavigationBar, NotificationRenderer::drawBannercallback};
    ui->setOverlays(overlays, sizeof(overlays) / sizeof(overlays[0]));
    ui->setTargetFPS(60);
    ui->update();
}

void Screen::showNumberPicker(const char *message, uint32_t durationMs, uint8_t digits,
                              std::function<void(uint32_t)> bannerCallback)
{
#ifdef USE_EINK
    EINK_ADD_FRAMEFLAG(dispdev, DEMAND_FAST);
#endif
    strncpy(NotificationRenderer::alertBannerMessage, message, 255);
    NotificationRenderer::alertBannerMessage[255] = '\0';
    NotificationRenderer::alertBannerUntil = (durationMs == 0) ? 0 : millis() + durationMs;
    NotificationRenderer::alertBannerCallback = bannerCallback;
    NotificationRenderer::pauseBanner = false;
    NotificationRenderer::curSelected = 0;
    NotificationRenderer::current_notification_type = notificationTypeEnum::number_picker;
    NotificationRenderer::numDigits = digits;
    NotificationRenderer::currentNumber = 0;

    static OverlayCallback overlays[] = {graphics::UIRenderer::drawNavigationBar, NotificationRenderer::drawBannercallback};
    ui->setOverlays(overlays, sizeof(overlays) / sizeof(overlays[0]));
    ui->setTargetFPS(60);
    ui->update();
}

void Screen::showTextInput(const char *header, const char *initialText, uint32_t durationMs,
                           std::function<void(const std::string &)> textCallback)
{
    LOG_INFO("showTextInput called with header='%s', durationMs=%d", header ? header : "NULL", durationMs);
	
	// Recordar el frame actual para volver después de enviar
	if (ui && ui->getUiState()) {
		s_returnToFrame    = ui->getUiState()->currentFrame;
		s_reFocusAfterSend = true;
	}

	
    if (NotificationRenderer::virtualKeyboard) {
        delete NotificationRenderer::virtualKeyboard;
        NotificationRenderer::virtualKeyboard = nullptr;
    }

    NotificationRenderer::textInputCallback = nullptr;

    NotificationRenderer::virtualKeyboard = new VirtualKeyboard();
    if (header) {
        NotificationRenderer::virtualKeyboard->setHeader(header);
    }
    if (initialText) {
        NotificationRenderer::virtualKeyboard->setInputText(initialText);
    }

    // === Aplica header pendiente aquí (último) ===
    if (!g_pendingKeyboardHeader.empty()) {
        std::string hdr = g_pendingKeyboardHeader;

        // límite de 11 para no pisar "xxxleft"
        const int cap = 11;

        if ((int)hdr.size() > cap) {
            static ScrollState g_headerScroll;
            std::string view = marqueeSlice(hdr, g_headerScroll, cap, true);
            NotificationRenderer::virtualKeyboard->setHeader(view.c_str());

            // 🔥 mantenemos el header para que siga haciendo scroll
            g_chatScrollActive = true;
        } else {
            NotificationRenderer::virtualKeyboard->setHeader(hdr.c_str());
            // solo limpiamos si es corto, ya no se necesita más
            g_pendingKeyboardHeader.clear();
        }
    }

    // Envolver el envío para volver al chat y evitar salto a “home”
auto wrappedSend = [this, textCallback](const std::string &text) {
    // 1) enviar (lo hace el callback original)
    textCallback(text);

    // 2) volver inmediatamente al frame de chat que teníamos
    if (s_returnToFrame >= 0) {
        ui->switchToFrame((uint8_t)s_returnToFrame);
        setFastFramerate();
        forceDisplay(true);

        // 3) marcar refocus por si otro setFrames ocurre después
        s_reFocusAfterSend = true;
    }
};

NotificationRenderer::textInputCallback = wrappedSend;
NotificationRenderer::virtualKeyboard->setCallback(wrappedSend);


    strncpy(NotificationRenderer::alertBannerMessage, header ? header : "Text Input", 255);
    NotificationRenderer::alertBannerMessage[255] = '\0';
    NotificationRenderer::alertBannerUntil = (durationMs == 0) ? 0 : millis() + durationMs;
    NotificationRenderer::pauseBanner = false;
    NotificationRenderer::current_notification_type = notificationTypeEnum::text_input;

    static OverlayCallback overlays[] = {graphics::UIRenderer::drawNavigationBar, NotificationRenderer::drawBannercallback};
    ui->setOverlays(overlays, sizeof(overlays) / sizeof(overlays[0]));
    ui->setTargetFPS(60);
    ui->update();
}

static void drawModuleFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    uint8_t module_frame;
    if (state->frameState == IN_TRANSITION && state->transitionFrameRelationship == TransitionRelationship_INCOMING) {
        module_frame = state->transitionFrameTarget;
    } else {
        module_frame = state->currentFrame;
    }
    MeshModule &pi = *moduleFrames.at(module_frame);
    pi.drawFrame(display, state, x, y);
}

static bool shouldDrawMessage(const meshtastic_MeshPacket *packet)
{
    return packet->from != 0 && !moduleConfig.store_forward.enabled;
}

float Screen::estimatedHeading(double lat, double lon)
{
    static double oldLat, oldLon;
    static float b;

    if (oldLat == 0) {
        oldLat = lat;
        oldLon = lon;
        return b;
    }

    float d = GeoCoord::latLongToMeter(oldLat, oldLon, lat, lon);
    if (d < 10) return b;

    b = GeoCoord::bearing(oldLat, oldLon, lat, lon) * RAD_TO_DEG;
    oldLat = lat;
    oldLon = lon;

    return b;
}

static int8_t prevFrame = -1;

#if defined(ESP_PLATFORM) && defined(USE_ST7789)
SPIClass SPI1(HSPI);
#endif

Screen::Screen(ScanI2C::DeviceAddress address, meshtastic_Config_DisplayConfig_OledType screenType, OLEDDISPLAY_GEOMETRY geometry)
    : concurrency::OSThread("Screen"), address_found(address), model(screenType), geometry(geometry), cmdQueue(32)
{
    graphics::normalFrames = new FrameCallback[MAX_NUM_NODES + NUM_EXTRA_FRAMES];

    LOG_INFO("Protobuf Value uiconfig.screen_rgb_color: %d", uiconfig.screen_rgb_color);
    int32_t rawRGB = uiconfig.screen_rgb_color;
    if (rawRGB > 0 && rawRGB <= 255255255) {
        uint8_t TFT_MESH_r = (rawRGB >> 16) & 0xFF;
        uint8_t TFT_MESH_g = (rawRGB >> 8) & 0xFF;
        uint8_t TFT_MESH_b = (rawRGB) & 0xFF;
        TFT_MESH = COLOR565(TFT_MESH_r, TFT_MESH_g, TFT_MESH_b);
    }

#if defined(USE_SH1106) || defined(USE_SH1107) || defined(USE_SH1107_128_64)
    dispdev = new SH1106Wire(address.address, -1, -1, geometry,
                             (address.port == ScanI2C::I2CPort::WIRE1) ? HW_I2C::I2C_TWO : HW_I2C::I2C_ONE);
#elif defined(USE_ST7789)
#ifdef ESP_PLATFORM
    dispdev = new ST7789Spi(&SPI1, ST7789_RESET, ST7789_RS, ST7789_NSS, GEOMETRY_RAWMODE, TFT_WIDTH, TFT_HEIGHT, ST7789_SDA,
                            ST7789_MISO, ST7789_SCK);
#else
    dispdev = new ST7789Spi(&SPI1, ST7789_RESET, ST7789_RS, ST7789_NSS, GEOMETRY_RAWMODE, TFT_WIDTH, TFT_HEIGHT);
#endif
    static_cast<ST7789Spi *>(dispdev)->setRGB(TFT_MESH);
#elif defined(USE_SSD1306)
    dispdev = new SSD1306Wire(address.address, -1, -1, geometry,
                              (address.port == ScanI2C::I2CPort::WIRE1) ? HW_I2C::I2C_TWO : HW_I2C::I2C_ONE);
#elif defined(ST7735_CS) || defined(ILI9341_DRIVER) || defined(ILI9342_DRIVER) || defined(ST7701_CS) || defined(ST7789_CS) ||    \
    defined(RAK14014) || defined(HX8357_CS) || defined(ILI9488_CS) || defined(ST7796_CS)
    dispdev = new TFTDisplay(address.address, -1, -1, geometry,
                             (address.port == ScanI2C::I2CPort::WIRE1) ? HW_I2C::I2C_TWO : HW_I2C::I2C_ONE);
#elif defined(USE_EINK) && !defined(USE_EINK_DYNAMICDISPLAY)
    dispdev = new EInkDisplay(address.address, -1, -1, geometry,
                              hwPortFrom(address.port));
#elif defined(USE_EINK) && defined(USE_EINK_DYNAMICDISPLAY)
    dispdev = new EInkDynamicDisplay(address.address, -1, -1, geometry,
                                     hwPortFrom(address.port);
#elif defined(USE_ST7567)
    dispdev = new ST7567Wire(address.address, -1, -1, geometry,
                             hwPortFrom(address.port);
#elif ARCH_PORTDUINO
    if (config.display.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_COLOR) {
        if (settingsMap[displayPanel] != no_screen) {
            LOG_DEBUG("Make TFTDisplay!");
            dispdev = new TFTDisplay(address.address, -1, -1, geometry,
                                     hwPortFrom(address.port));
        } else {
            dispdev = new AutoOLEDWire(address.address, -1, -1, geometry,
                                       hwPortFrom(address.port));
            isAUTOOled = true;
        }
    }
#else
    dispdev = new AutoOLEDWire(address.address, -1, -1, geometry,
                               hwPortFrom(address.port);
    isAUTOOled = true;
#endif

    ui = new OLEDDisplayUi(dispdev);
    cmdQueue.setReader(this);
}

Screen::~Screen()
{
    delete[] graphics::normalFrames;
}

/**
 * Prepare the display for deep sleep.
 */
void Screen::doDeepSleep()
{
#ifdef USE_EINK
    setOn(false, graphics::UIRenderer::drawDeepSleepFrame);
#else
    setOn(false);
#endif
}

void Screen::handleSetOn(bool on, FrameCallback einkScreensaver)
{
    if (!useDisplay)
        return;

    if (on != screenOn) {
        if (on) {
            LOG_INFO("Turn on screen");
            powerMon->setState(meshtastic_PowerMon_State_Screen_On);
#ifdef T_WATCH_S3
            PMU->enablePowerOutput(XPOWERS_ALDO2);
#endif

#if !ARCH_PORTDUINO
            dispdev->displayOn();
#endif

#ifdef PIN_EINK_EN
            if (uiconfig.screen_brightness == 1)
                digitalWrite(PIN_EINK_EN, HIGH);
#elif defined(PCA_PIN_EINK_EN)
            if (uiconfig.screen_brightness == 1)
                io.digitalWrite(PCA_PIN_EINK_EN, HIGH);
#endif

#if defined(ST7789_CS) && !defined(M5STACK)
            static_cast<TFTDisplay *>(dispdev)->setDisplayBrightness(brightness);
#endif

            dispdev->displayOn();
#ifdef HELTEC_TRACKER_V1_X
            ui->init();
#endif
#ifdef USE_ST7789
            pinMode(VTFT_CTRL, OUTPUT);
            digitalWrite(VTFT_CTRL, LOW);
            ui->init();
#ifdef ESP_PLATFORM
            analogWrite(VTFT_LEDA, BRIGHTNESS_DEFAULT);
#else
            pinMode(VTFT_LEDA, OUTPUT);
            digitalWrite(VTFT_LEDA, TFT_BACKLIGHT_ON);
#endif
#endif
            enabled = true;
            setInterval(0);
            runASAP = true;
        } else {
            powerMon->clearState(meshtastic_PowerMon_State_Screen_On);
#ifdef USE_EINK
            setScreensaverFrames(einkScreensaver);
#endif

#ifdef PIN_EINK_EN
            digitalWrite(PIN_EINK_EN, LOW);
#elif defined(PCA_PIN_EINK_EN)
            io.digitalWrite(PCA_PIN_EINK_EN, LOW);
#endif

            dispdev->displayOff();
#ifdef USE_ST7789
            SPI1.end();
#if defined(ARCH_ESP32)
            pinMode(VTFT_LEDA, ANALOG);
            pinMode(VTFT_CTRL, ANALOG);
            pinMode(ST7789_RESET, ANALOG);
            pinMode(ST7789_RS, ANALOG);
            pinMode(ST7789_NSS, ANALOG);
#else
            nrf_gpio_cfg_default(VTFT_LEDA);
            nrf_gpio_cfg_default(VTFT_CTRL);
            nrf_gpio_cfg_default(ST7789_RESET);
            nrf_gpio_cfg_default(ST7789_RS);
            nrf_gpio_cfg_default(ST7789_NSS);
#endif
#endif

#ifdef T_WATCH_S3
            PMU->disablePowerOutput(XPOWERS_ALDO2);
#endif
            enabled = false;
        }
        screenOn = on;
    }
}

void Screen::setup()
{
    useDisplay = true;

    if (uiconfig.screen_brightness == 0) {
#if defined(USE_OLED) || defined(USE_SSD1306) || defined(USE_SH1106) || defined(USE_SH1107)
        brightness = 255;
#else
        brightness = BRIGHTNESS_DEFAULT;
#endif
    } else {
        brightness = uiconfig.screen_brightness;
    }

#ifdef AutoOLEDWire_h
    if (isAUTOOled)
        static_cast<AutoOLEDWire *>(dispdev)->setDetected(model);
#endif

#ifdef USE_SH1107_128_64
    static_cast<SH1106Wire *>(dispdev)->setSubtype(7);
#endif

#if defined(USE_ST7789) && defined(TFT_MESH)
    static_cast<ST7789Spi *>(dispdev)->setRGB(TFT_MESH);
#endif

    ui->init();
    displayWidth = dispdev->width();
    displayHeight = dispdev->height();

    ui->setTimePerTransition(0);
    ui->setIndicatorPosition(BOTTOM);
    ui->setIndicatorDirection(LEFT_RIGHT);
    ui->setFrameAnimation(SLIDE_LEFT);
    ui->disableAllIndicators();
    ui->getUiState()->userData = this;

#if defined(ST7789_CS)
    static_cast<TFTDisplay *>(dispdev)->setDisplayBrightness(brightness);
#elif defined(USE_OLED) || defined(USE_SSD1306) || defined(USE_SH1106) || defined(USE_SH1107)
    dispdev->setBrightness(brightness);
#endif

    dispdev->setFontTableLookupFunction(customFontTableLookup);

#ifdef USERPREFS_OEM_TEXT
    logo_timeout *= 2;
#endif

    EINK_ADD_FRAMEFLAG(dispdev, DEMAND_FAST);
    alertFrames[0] = [this](OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) {
#ifdef ARCH_ESP32
        if (wakeCause == ESP_SLEEP_WAKEUP_TIMER || wakeCause == ESP_SLEEP_WAKEUP_EXT1)
            graphics::UIRenderer::drawFrameText(display, state, x, y, "Resuming...");
        else
#endif
        {
            const char *region = myRegion ? myRegion->name : nullptr;
            graphics::UIRenderer::drawIconScreen(region, display, state, x, y);
        }
    };
    ui->setFrames(alertFrames, 1);
    ui->disableAutoTransition();

    dispdev->setLogBuffer(3, 32);

#ifdef SCREEN_MIRROR
    dispdev->mirrorScreen();
#else
    if (!config.display.flip_screen) {
#if defined(ST7701_CS) || defined(ST7735_CS) || defined(ILI9341_DRIVER) || defined(ILI9342_DRIVER) || defined(ST7789_CS) ||      \
    defined(RAK14014) || defined(HX8357_CS) || defined(ILI9488_CS) || defined(ST7796_CS)
        static_cast<TFTDisplay *>(dispdev)->flipScreenVertically();
#elif defined(USE_ST7789)
        static_cast<ST7789Spi *>(dispdev)->flipScreenVertically();
#else
        dispdev->flipScreenVertically();
#endif
    }
#endif

    uint8_t dmac[6];
    getMacAddr(dmac);
    snprintf(screen->ourId, sizeof(screen->ourId), "%02x%02x", dmac[4], dmac[5]);

#if ARCH_PORTDUINO
    handleSetOn(false);
#endif

    handleSetOn(true);
    determineResolution(dispdev->height(), dispdev->width());
    ui->update();
#ifndef USE_EINK
    ui->update();
#endif
    serialSinceMsec = millis();

#if ARCH_PORTDUINO
    if (config.display.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_COLOR) {
        if (settingsMap[touchscreenModule]) {
            touchScreenImpl1 =
                new TouchScreenImpl1(dispdev->getWidth(), dispdev->getHeight(), static_cast<TFTDisplay *>(dispdev)->getTouch);
            touchScreenImpl1->init();
        }
    }
#elif HAS_TOUCHSCREEN && !defined(USE_EINK)
    touchScreenImpl1 =
        new TouchScreenImpl1(dispdev->getWidth(), dispdev->getHeight(), static_cast<TFTDisplay *>(dispdev)->getTouch);
    touchScreenImpl1->init();
#endif

    powerStatusObserver.observe(&powerStatus->onNewStatus);
    gpsStatusObserver.observe(&gpsStatus->onNewStatus);
    nodeStatusObserver.observe(&nodeStatus->onNewStatus);

#if !MESHTASTIC_EXCLUDE_ADMIN
    adminMessageObserver.observe(adminModule);
#endif
    if (textMessageModule)
        textMessageObserver.observe(textMessageModule);
    if (inputBroker)
        inputObserver.observe(inputBroker);

    MeshModule::observeUIEvents(&uiFrameEventObserver);
}

void Screen::forceDisplay(bool forceUiUpdate)
{
#ifdef USE_EINK
    if (forceUiUpdate) {
        EINK_ADD_FRAMEFLAG(dispdev, DEMAND_FAST);
        setFastFramerate();
        while (!cmdQueue.isEmpty())
            runOnce();
        uint64_t startUpdate;
        do {
            startUpdate = millis();
            delay(10);
            ui->update();
        } while (ui->getUiState()->lastUpdate < startUpdate);
        targetFramerate = IDLE_FRAMERATE;
        ui->setTargetFPS(targetFramerate);
    }
    static_cast<EInkDisplay *>(dispdev)->forceDisplay();
#else
    if (forceUiUpdate) {
        setFastFramerate();
    }
#endif
}

static uint32_t lastScreenTransition;

int32_t Screen::runOnce()
{
    if (!useDisplay) {
        enabled = false;
        return RUN_SAME;
    }

    if (displayHeight == 0) {
        displayHeight = dispdev->getHeight();
    }
    menuHandler::handleMenuSwitch(dispdev);

    static bool showingBootScreen = true;
    if (showingBootScreen && (millis() > (logo_timeout + serialSinceMsec))) {
        LOG_INFO("Done with boot screen");
        stopBootScreen();
        showingBootScreen = false;
    }

#ifdef USERPREFS_OEM_TEXT
    static bool showingOEMBootScreen = true;
    if (showingOEMBootScreen && (millis() > ((logo_timeout / 2) + serialSinceMsec))) {
        LOG_INFO("Switch to OEM screen...");
        static FrameCallback bootOEMFrames[] = {graphics::UIRenderer::drawOEMBootScreen};
        static const int bootOEMFrameCount = sizeof(bootOEMFrames) / sizeof(bootOEMFrames[0]);
        ui->setFrames(bootOEMFrames, bootOEMFrameCount);
        ui->update();
#ifndef USE_EINK
        ui->update();
#endif
        showingOEMBootScreen = false;
    }
#endif

#ifndef DISABLE_WELCOME_UNSET
    if (!NotificationRenderer::isOverlayBannerShowing() && config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        menuHandler::OnboardMessage();
    }
#endif
    if (!NotificationRenderer::isOverlayBannerShowing() && rebootAtMsec != 0) {
        showSimpleBanner("Rebooting...", 0);
    }

    for (;;) {
        ScreenCmd cmd;
        if (!cmdQueue.dequeue(&cmd, 0)) {
            break;
        }
        switch (cmd.cmd) {
        case Cmd::SET_ON:
            handleSetOn(true);
            break;
        case Cmd::SET_OFF:
            handleSetOn(false);
            break;
        case Cmd::ON_PRESS:
            if (NotificationRenderer::current_notification_type != notificationTypeEnum::text_input) {
                handleOnPress();
            }
            break;
        case Cmd::SHOW_PREV_FRAME:
            if (NotificationRenderer::current_notification_type != notificationTypeEnum::text_input) {
                handleShowPrevFrame();
            }
            break;
        case Cmd::SHOW_NEXT_FRAME:
            if (NotificationRenderer::current_notification_type != notificationTypeEnum::text_input) {
                handleShowNextFrame();
            }
            break;
        case Cmd::START_ALERT_FRAME: {
            showingNormalScreen = false;
            NotificationRenderer::pauseBanner = true;
            alertFrames[0] = alertFrame;
#ifdef USE_EINK
            EINK_ADD_FRAMEFLAG(dispdev, DEMAND_FAST);
            EINK_ADD_FRAMEFLAG(dispdev, BLOCKING);
            handleSetOn(true);
#endif
            setFrameImmediateDraw(alertFrames);
            break;
        }
        case Cmd::START_FIRMWARE_UPDATE_SCREEN:
            handleStartFirmwareUpdateScreen();
            break;
                case Cmd::STOP_ALERT_FRAME:
            NotificationRenderer::pauseBanner = false;
            // fall through
        case Cmd::STOP_BOOT_SCREEN:
            EINK_ADD_FRAMEFLAG(dispdev, COSMETIC);
            // Al cerrar overlays que NO son text_input, preservamos el frame actual
            if (NotificationRenderer::current_notification_type != notificationTypeEnum::text_input) {
                setFrames(FOCUS_PRESERVE);
            }
            break;
        case Cmd::NOOP:
            break;
        default:
            LOG_ERROR("Invalid screen cmd");
        }
    }

    if (!screenOn) {
        enabled = false;
        return 0;
    }

    // === ciclo de dibujo ===
    g_chatScrollActive = false;  // los draw* lo pondrán a true si hay marquee
    ui->update();

    // Gestionar FPS según haya marquee activo o no
    if (ui->getUiState()->frameState == FIXED) {
        if (g_chatScrollActive) {
            if (targetFramerate == IDLE_FRAMERATE) {
                setFastFramerate();
            }
        } else if (targetFramerate != IDLE_FRAMERATE) {
            targetFramerate = IDLE_FRAMERATE;
            ui->setTargetFPS(targetFramerate);
            forceDisplay();
        }
    }

    if (showingNormalScreen) {
        if (config.display.auto_screen_carousel_secs > 0 &&
            NotificationRenderer::current_notification_type != notificationTypeEnum::text_input &&
            !Throttle::isWithinTimespanMs(lastScreenTransition, config.display.auto_screen_carousel_secs * 1000)) {
#if !defined(EINK_BACKGROUND_USES_FAST)
            EINK_ADD_FRAMEFLAG(dispdev, COSMETIC);
#endif
            LOG_DEBUG("LastScreenTransition exceeded %ums transition to next frame", (millis() - lastScreenTransition));
            handleOnPress();
        }
    }

    return (1000 / targetFramerate);
}

void Screen::setSSLFrames()
{
    if (address_found.address) {
        static FrameCallback sslFrames[] = {NotificationRenderer::drawSSLScreen};
        ui->setFrames(sslFrames, 1);
        ui->update();
    }
}

#ifdef USE_EINK
void Screen::setScreensaverFrames(FrameCallback einkScreensaver)
{
    static FrameCallback screensaverFrame;
    static OverlayCallback screensaverOverlay;

#if defined(HAS_EINK_ASYNCFULL) && defined(USE_EINK_DYNAMICDISPLAY)
    EINK_JOIN_ASYNCREFRESH(dispdev);
#endif

    if (einkScreensaver != NULL) {
        screensaverFrame = einkScreensaver;
        ui->setFrames(&screensaverFrame, 1);
    } else {
        screensaverOverlay = graphics::UIRenderer::drawScreensaverOverlay;
        ui->setOverlays(&screensaverOverlay, 1);
    }

    setFastFramerate();
    uint64_t startUpdate;
    do {
        startUpdate = millis();
        delay(1);
        ui->update();
    } while (ui->getUiState()->lastUpdate < startUpdate);

#if !defined(USE_EINK_DYNAMICDISPLAY)
    static_cast<EInkDisplay *>(dispdev)->forceDisplay(0);
#endif

    ui->setOverlays(NULL, 0);
    setFrames(FOCUS_PRESERVE);

#ifdef EINK_HASQUIRK_GHOSTING
    EINK_ADD_FRAMEFLAG(dispdev, COSMETIC);
#else
    EINK_ADD_FRAMEFLAG(dispdev, RESPONSIVE);
#endif
}
#endif

// Regenerate frames
void Screen::setFrames(FrameFocus focus)
{
    if (NotificationRenderer::current_notification_type == notificationTypeEnum::text_input) {
        return;
    }

    uint8_t originalPosition = ui->getUiState()->currentFrame;
    uint8_t previousFrameCount = framesetInfo.frameCount;
    FramesetInfo fsi;

    graphics::UIRenderer::rebuildFavoritedNodes();

    LOG_DEBUG("Show standard frames");
    showingNormalScreen = true;

    indicatorIcons.clear();

    size_t numframes = 0;

    fsi.positions.fault = numframes;
    if (error_code) {
        normalFrames[numframes++] = NotificationRenderer::drawCriticalFaultFrame;
        indicatorIcons.push_back(icon_error);
        focus = FOCUS_FAULT;
    }

#if defined(DISPLAY_CLOCK_FRAME)
    if (!hiddenFrames.clock) {
        fsi.positions.clock = numframes;
        normalFrames[numframes++] = uiconfig.is_clockface_analog ? graphics::ClockRenderer::drawAnalogClockFrame
                                                                 : graphics::ClockRenderer::drawDigitalClockFrame;
        indicatorIcons.push_back(digital_icon_clock);
    }
#endif

    if (!hiddenFrames.home) {
        fsi.positions.home = numframes;
        normalFrames[numframes++] = graphics::UIRenderer::drawDeviceFocused;
        indicatorIcons.push_back(icon_home);
    }

    //fsi.positions.textMessage = numframes;
    //normalFrames[numframes++] = graphics::MessageRenderer::drawTextMessageFrame;
    //indicatorIcons.push_back(icon_mail);

#ifndef USE_EINK
    if (!hiddenFrames.nodelist) {
        fsi.positions.nodelist = numframes;
        normalFrames[numframes++] = graphics::NodeListRenderer::drawDynamicNodeListScreen;
        indicatorIcons.push_back(icon_nodes);
    }
#endif

#ifdef USE_EINK
    if (!hiddenFrames.nodelist_lastheard) {
        fsi.positions.nodelist_lastheard = numframes;
        normalFrames[numframes++] = graphics::NodeListRenderer::drawLastHeardScreen;
        indicatorIcons.push_back(icon_nodes);
    }
    if (!hiddenFrames.nodelist_hopsignal) {
        fsi.positions.nodelist_hopsignal = numframes;
        normalFrames[numframes++] = graphics::NodeListRenderer::drawHopSignalScreen;
        indicatorIcons.push_back(icon_signal);
    }
    if (!hiddenFrames.nodelist_distance) {
        fsi.positions.nodelist_distance = numframes;
        normalFrames[numframes++] = graphics::NodeListRenderer::drawDistanceScreen;
        indicatorIcons.push_back(icon_distance);
    }
#endif
#if HAS_GPS
    if (!hiddenFrames.nodelist_bearings) {
        fsi.positions.nodelist_bearings = numframes;
        normalFrames[numframes++] = graphics::NodeListRenderer::drawNodeListWithCompasses;
        indicatorIcons.push_back(icon_list);
    }
    if (!hiddenFrames.gps) {
        fsi.positions.gps = numframes;
        normalFrames[numframes++] = graphics::UIRenderer::drawCompassAndLocationScreen;
        indicatorIcons.push_back(icon_compass);
    }
#endif
    if (RadioLibInterface::instance && !hiddenFrames.lora) {
        fsi.positions.lora = numframes;
        normalFrames[numframes++] = graphics::DebugRenderer::drawLoRaFocused;
        indicatorIcons.push_back(icon_radio);
    }
    if (!hiddenFrames.system) {
        fsi.positions.system = numframes;
        normalFrames[numframes++] = graphics::DebugRenderer::drawSystemScreen;
        indicatorIcons.push_back(icon_system);
    }
#if !defined(DISPLAY_CLOCK_FRAME)
    if (!hiddenFrames.clock) {
        fsi.positions.clock = numframes;
        normalFrames[numframes++] = uiconfig.is_clockface_analog ? graphics::ClockRenderer::drawAnalogClockFrame
                                                                 : graphics::ClockRenderer::drawDigitalClockFrame;
        indicatorIcons.push_back(digital_icon_clock);
    }
#endif

#if HAS_WIFI && !defined(ARCH_PORTDUINO)
    if (!hiddenFrames.wifi && isWifiAvailable()) {
        fsi.positions.wifi = numframes;
        normalFrames[numframes++] = graphics::DebugRenderer::drawDebugInfoWiFiTrampoline;
        indicatorIcons.push_back(icon_wifi);
    }
#endif

    // Módulos
    moduleFrames = MeshModule::GetMeshModulesWithUIFrames(numframes);
    LOG_DEBUG("Show %d module frames", moduleFrames.size());

    for (auto i = moduleFrames.begin(); i != moduleFrames.end(); ++i) {
        if (*i != nullptr) {
            normalFrames[numframes] = drawModuleFrame;

            MeshModule *m = *i;
            if (m && m->isRequestingFocus())
                fsi.positions.focusedModule = numframes;
            if (m && m == waypointModule)
                fsi.positions.waypoint = numframes;

            indicatorIcons.push_back(icon_module);
            numframes++;
        }
    }

    LOG_DEBUG("Added modules.  numframes: %d", numframes);
	// ——— semilla de pestañas de canal al arrancar ———
		seedChannelTabsFromConfig();


    // ===== Pestañas de chat por nodo (favoritos) =====
    {
        graphics::g_favChatNodes.clear();
        for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
            const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
            if (n && n->num != nodeDB->getNodeNum() && n->is_favorite) {
                graphics::g_favChatNodes.push_back(n->num);
            }
        }
        if (!graphics::g_favChatNodes.empty()) {
            graphics::g_favChatFirst = numframes;
            for (size_t i = 0; i < graphics::g_favChatNodes.size(); ++i) {
                normalFrames[numframes++] = graphics::drawFavNodeChatFrame;
                indicatorIcons.push_back(icon_mail);
            }
            graphics::g_favChatLast = numframes - 1;
        } else {
            graphics::g_favChatFirst = graphics::g_favChatLast = (size_t)-1;
        }
    }

    // ===== Pestañas de chat por canal =====
    {
        using chat::ChatHistoryStore;
        auto &store = ChatHistoryStore::instance();

        // Unir historial con favoritos de canal gestionados aquí
        std::set<uint8_t> combined;
        std::vector<uint8_t> fromHistory = store.listChannels();
        combined.insert(fromHistory.begin(), fromHistory.end());
        combined.insert(g_favChannelTabs.begin(), g_favChannelTabs.end());

        graphics::g_chanTabs.assign(combined.begin(), combined.end());

        if (!graphics::g_chanTabs.empty()) {
            graphics::g_chanTabFirst = numframes;
            for (size_t i = 0; i < graphics::g_chanTabs.size(); ++i) {
                normalFrames[numframes++] = graphics::drawChannelChatTabFrame;
                indicatorIcons.push_back(icon_mail);
            }
            graphics::g_chanTabLast = numframes - 1;
        } else {
            graphics::g_chanTabFirst = graphics::g_chanTabLast = (size_t)-1;
        }
    }

    fsi.frameCount = numframes;
    this->frameCount = numframes;
    LOG_DEBUG("Finished build frames. numframes: %d", numframes);

    ui->setFrames(normalFrames, numframes);
    ui->disableAllIndicators();

    static OverlayCallback overlays[] = {graphics::UIRenderer::drawNavigationBar, NotificationRenderer::drawBannercallback};
    ui->setOverlays(overlays, sizeof(overlays) / sizeof(overlays[0]));

    prevFrame = -1;

    switch (focus) {
    case FOCUS_DEFAULT:
        ui->switchToFrame(fsi.positions.deviceFocused);
        break;
    case FOCUS_FAULT:
        ui->switchToFrame(fsi.positions.fault);
        break;
    case FOCUS_TEXTMESSAGE:
        hasUnreadMessage = false;
        ui->switchToFrame(fsi.positions.textMessage);
        break;
    case FOCUS_MODULE:
        ui->switchToFrame(fsi.positions.focusedModule);
        break;
    case FOCUS_CLOCK:
        ui->switchToFrame(fsi.positions.clock);
        break;
    case FOCUS_SYSTEM:
        ui->switchToFrame(fsi.positions.system);
        break;
    case FOCUS_PRESERVE:
        if (previousFrameCount > fsi.frameCount) {
            ui->switchToFrame(originalPosition - 1);
        } else if (previousFrameCount < fsi.frameCount) {
            ui->switchToFrame(originalPosition + 1);
        } else {
            ui->switchToFrame(originalPosition);
        }
        break;
    }

    this->framesetInfo = fsi;
	
	// Si acabamos de enviar texto, forzar volver al frame guardado
if (s_reFocusAfterSend && s_returnToFrame >= 0) {
    uint8_t target = (uint8_t)std::min<int>(s_returnToFrame, (int)frameCount - 1);
    ui->switchToFrame(target);
    s_reFocusAfterSend = false;
    s_returnToFrame    = -1;
}


    setFastFramerate();
}

void Screen::setFrameImmediateDraw(FrameCallback *drawFrames)
{
    ui->disableAllIndicators();
    ui->setFrames(drawFrames, 1);
    setFastFramerate();
}

void Screen::toggleFrameVisibility(const std::string &frameName)
{
#ifndef USE_EINK
    if (frameName == "nodelist") {
        hiddenFrames.nodelist = !hiddenFrames.nodelist;
    }
#endif
#ifdef USE_EINK
    if (frameName == "nodelist_lastheard") {
        hiddenFrames.nodelist_lastheard = !hiddenFrames.nodelist_lastheard;
    }
    if (frameName == "nodelist_hopsignal") {
        hiddenFrames.nodelist_hopsignal = !hiddenFrames.nodelist_hopsignal;
    }
    if (frameName == "nodelist_distance") {
        hiddenFrames.nodelist_distance = !hiddenFrames.nodelist_distance;
    }
#endif
#if HAS_GPS
    if (frameName == "nodelist_bearings") {
        hiddenFrames.nodelist_bearings = !hiddenFrames.nodelist_bearings;
    }
    if (frameName == "gps") {
        hiddenFrames.gps = !hiddenFrames.gps;
    }
#endif
    if (frameName == "lora") {
        hiddenFrames.lora = !hiddenFrames.lora;
    }
    if (frameName == "clock") {
        hiddenFrames.clock = !hiddenFrames.clock;
    }
    if (frameName == "show_favorites") {
        hiddenFrames.show_favorites = !hiddenFrames.show_favorites;
    }
}

bool Screen::isFrameHidden(const std::string &frameName) const
{
#ifndef USE_EINK
    if (frameName == "nodelist")
        return hiddenFrames.nodelist;
#endif
#ifdef USE_EINK
    if (frameName == "nodelist_lastheard")
        return hiddenFrames.nodelist_lastheard;
    if (frameName == "nodelist_hopsignal")
        return hiddenFrames.nodelist_hopsignal;
    if (frameName == "nodelist_distance")
        return hiddenFrames.nodelist_distance;
#endif
#if HAS_GPS
    if (frameName == "nodelist_bearings")
        return hiddenFrames.nodelist_bearings;
    if (frameName == "gps")
        return hiddenFrames.gps;
#endif
    if (frameName == "lora")
        return hiddenFrames.lora;
    if (frameName == "clock")
        return hiddenFrames.clock;
    if (frameName == "show_favorites")
        return hiddenFrames.show_favorites;

    return false;
}

void Screen::hideCurrentFrame()
{
    uint8_t currentFrame = ui->getUiState()->currentFrame;
    bool dismissed = false;
    if (currentFrame == framesetInfo.positions.textMessage && devicestate.has_rx_text_message) {
        LOG_INFO("Hide Text Message");
        devicestate.has_rx_text_message = false;
        memset(&devicestate.rx_text_message, 0, sizeof(devicestate.rx_text_message));
        hiddenFrames.textMessage = true;
        dismissed = true;
    } else if (currentFrame == framesetInfo.positions.waypoint && devicestate.has_rx_waypoint) {
        LOG_DEBUG("Hide Waypoint");
        devicestate.has_rx_waypoint = false;
        hiddenFrames.waypoint = true;
        dismissed = true;
    } else if (currentFrame == framesetInfo.positions.wifi) {
        LOG_DEBUG("Hide WiFi Screen");
        hiddenFrames.wifi = true;
        dismissed = true;
    } else if (currentFrame == framesetInfo.positions.lora) {
        LOG_INFO("Hide LoRa");
        hiddenFrames.lora = true;
        dismissed = true;
    }

    if (dismissed) {
        setFrames(FOCUS_DEFAULT);
    }
}

void Screen::handleStartFirmwareUpdateScreen()
{
    LOG_DEBUG("Show firmware screen");
    showingNormalScreen = false;
    EINK_ADD_FRAMEFLAG(dispdev, DEMAND_FAST);

    static FrameCallback frames[] = {graphics::NotificationRenderer::drawFrameFirmware};
    setFrameImmediateDraw(frames);
}

void Screen::blink()
{
    setFastFramerate();
    uint8_t count = 10;
    dispdev->setBrightness(254);
    while (count > 0) {
        dispdev->fillRect(0, 0, dispdev->getWidth(), dispdev->getHeight());
        dispdev->display();
        delay(50);
        dispdev->clear();
        dispdev->display();
        delay(50);
        count = count - 1;
    }
    dispdev->setBrightness(brightness);
}

void Screen::increaseBrightness()
{
    brightness = ((brightness + 62) > 254) ? brightness : (brightness + 62);

#if defined(ST7789_CS)
    static_cast<TFTDisplay *>(dispdev)->setDisplayBrightness(brightness);
#endif
}

void Screen::decreaseBrightness()
{
    brightness = (brightness < 70) ? brightness : (brightness - 62);

#if defined(ST7789_CS)
    static_cast<TFTDisplay *>(dispdev)->setDisplayBrightness(brightness);
#endif
}

void Screen::setFunctionSymbol(std::string sym)
{
    if (std::find(functionSymbol.begin(), functionSymbol.end(), sym) == functionSymbol.end()) {
        functionSymbol.push_back(sym);
        functionSymbolString = "";
        for (auto symbol : functionSymbol) {
            functionSymbolString = symbol + " " + functionSymbolString;
        }
        setFastFramerate();
    }
}

void Screen::removeFunctionSymbol(std::string sym)
{
    functionSymbol.erase(std::remove(functionSymbol.begin(), functionSymbol.end(), sym), functionSymbol.end());
    functionSymbolString = "";
    for (auto symbol : functionSymbol) {
        functionSymbolString = symbol + " " + functionSymbolString;
    }
    setFastFramerate();
}

void Screen::handleOnPress()
{
    if (ui->getUiState()->frameState == FIXED) {
        ui->nextFrame();
        lastScreenTransition = millis();
        setFastFramerate();
    }
}

void Screen::handleShowPrevFrame()
{
    if (ui->getUiState()->frameState == FIXED) {
        ui->previousFrame();
        lastScreenTransition = millis();
        setFastFramerate();
    }
}

void Screen::handleShowNextFrame()
{
    if (ui->getUiState()->frameState == FIXED) {
        ui->nextFrame();
        lastScreenTransition = millis();
        setFastFramerate();
    }
}

#ifndef SCREEN_TRANSITION_FRAMERATE
#define SCREEN_TRANSITION_FRAMERATE 30 // fps
#endif

void Screen::setFastFramerate()
{
    targetFramerate = SCREEN_TRANSITION_FRAMERATE;
    ui->setTargetFPS(targetFramerate);
    setInterval(0);
    runASAP = true;
}

int Screen::handleStatusUpdate(const meshtastic::Status *arg)
{
    switch (arg->getStatusType()) {
    case STATUS_TYPE_NODE:
        if (showingNormalScreen && nodeStatus->getLastNumTotal() != nodeStatus->getNumTotal()) {
            setFrames(FOCUS_PRESERVE);
        }
        nodeDB->updateGUI = false;
        break;
    }
    return 0;
}

int Screen::handleTextMessage(const meshtastic_MeshPacket *packet)
{
    if (showingNormalScreen) {
        if (packet->from == 0) {
            devicestate.has_rx_text_message = false;
            memset(&devicestate.rx_text_message, 0, sizeof(devicestate.rx_text_message));
            hiddenFrames.textMessage = true;
            hasUnreadMessage = false;
            setFrames(FOCUS_PRESERVE);
        } else {
            // === FAVORITOS: solo en DM (destino = mi NodeNum) ===
            const bool isDirect = (nodeDB && packet->to == nodeDB->getNodeNum());
            if (isDirect) {
                const uint32_t fromId = packet->from;
                if (nodeDB && fromId != nodeDB->getNodeNum()) {
                    const meshtastic_NodeInfoLite *cn = nodeDB->getMeshNode(fromId);
                    bool isFav = (cn && cn->is_favorite);
                    if (!isFav) {
                        nodeDB->set_favorite(fromId, true);
                        if (cn) { const_cast<meshtastic_NodeInfoLite *>(cn)->is_favorite = true; }
                    }
                }
            } else {
                // Mensaje de CANAL: opcionalmente marcamos el canal como favorito interno
                uint8_t ch = (uint8_t)packet->channel;
                g_favChannelTabs.insert(ch);
            }

            // Estado y refresco
            devicestate.has_rx_text_message = true;
            hasUnreadMessage = true;
            setFrames(FOCUS_PRESERVE);

            if (shouldWakeOnReceivedMessage()) setOn(true);

            // === SALTO DE PANTALLA ===
            uint8_t jumpTo = 0xFF;
            if (isDirect) {
                if (g_favChatFirst != (size_t)-1) {
                    auto it = std::find(g_favChatNodes.begin(), g_favChatNodes.end(), packet->from);
                    if (it != g_favChatNodes.end()) {
                        jumpTo = (uint8_t)(g_favChatFirst + (it - g_favChatNodes.begin()));
                    }
                }
            } else {
                if (g_chanTabFirst != (size_t)-1) {
                    uint8_t ch = (uint8_t)packet->channel;
                    auto itc = std::find(g_chanTabs.begin(), g_chanTabs.end(), ch);
                    if (itc != g_chanTabs.end()) {
                        jumpTo = (uint8_t)(g_chanTabFirst + (itc - g_chanTabs.begin()));
                    }
                }
            }

            if (jumpTo != 0xFF) {
                ui->switchToFrame(jumpTo);
                setFastFramerate();
                forceDisplay();
            }

            // 👇 Banner eliminado: ya no muestra "New Message"
        }
    }
    return 0;
}

int Screen::handleUIFrameEvent(const UIFrameEvent *event)
{
    if (NotificationRenderer::current_notification_type == notificationTypeEnum::text_input) {
        return 0;
    }

    if (showingNormalScreen) {
        if (event->action == UIFrameEvent::Action::REGENERATE_FRAMESET)
            setFrames(FOCUS_MODULE);
        else if (event->action == UIFrameEvent::Action::REGENERATE_FRAMESET_BACKGROUND)
            setFrames(FOCUS_PRESERVE);
        else if (event->action == UIFrameEvent::Action::REDRAW_ONLY)
            setFastFramerate();
    }

    return 0;
}

// Helper: detectar pulsación larga de forma robusta en distintas placas
static inline bool isLongPressEvent(int ev) {
    switch (ev) {
        case INPUT_BROKER_SELECT_LONG:
            return true;
#ifdef INPUT_BROKER_USER_LONG
        case INPUT_BROKER_USER_LONG:
            return true;
#endif
#ifdef INPUT_BROKER_ALT_PRESS_LONG
        case INPUT_BROKER_ALT_PRESS_LONG:
            return true;
#endif
#ifdef INPUT_BROKER_USER_HOLD
        case INPUT_BROKER_USER_HOLD:
            return true;
#endif
#ifdef INPUT_BROKER_LONG_PRESS
        case INPUT_BROKER_LONG_PRESS:
            return true;
#endif
        default:
            return false;
    }
}


int Screen::handleInputEvent(const InputEvent *event)
{
	
    if (!screenOn)
        return 0;

    if (NotificationRenderer::current_notification_type == notificationTypeEnum::text_input) {
        NotificationRenderer::inEvent = *event;
        static OverlayCallback overlays[] = {graphics::UIRenderer::drawNavigationBar, NotificationRenderer::drawBannercallback};
        ui->setOverlays(overlays, sizeof(overlays) / sizeof(overlays[0]));
        setFastFramerate();
        ui->update();
        return 0;
    }

#ifdef USE_EINK
    EINK_ADD_FRAMEFLAG(dispdev, DEMAND_FAST);
    EINK_ADD_FRAMEFLAG(dispdev, BLOCKING);
    handleSetOn(true);
    setFastFramerate();
#endif
    if (NotificationRenderer::isOverlayBannerShowing()) {
        NotificationRenderer::inEvent = *event;
        static OverlayCallback overlays[] = {graphics::UIRenderer::drawNavigationBar, NotificationRenderer::drawBannercallback};
        ui->setOverlays(overlays, sizeof(overlays) / sizeof(overlays[0]));
        setFastFramerate();
        ui->update();

        menuHandler::handleMenuSwitch(dispdev);
        return 0;
    }

    if (showingNormalScreen) {
        bool inputIntercepted = false;
        for (MeshModule *module : moduleFrames) {
            if (module && module->interceptingKeyboardInput())
                inputIntercepted = true;
        }

        if (!inputIntercepted) {
            // === ¿Estamos en una pestaña de chat? ===
            uint8_t cf = this->ui->getUiState()->currentFrame;
            bool inNodeChat = (g_favChatFirst != (size_t)-1 && cf >= g_favChatFirst && cf <= g_favChatLast);
            bool inChanChat = (g_chanTabFirst != (size_t)-1 && cf >= g_chanTabFirst && cf <= g_chanTabLast);

            auto moveSelDM = [&](uint32_t nodeId, int dir) {
                const auto &q = chat::ChatHistoryStore::instance().getDM(nodeId);
                const int h = dispdev->getHeight();
                const int lineH = 10;
                const int maxLines = ((h - 16) / lineH > 4) ? ((h - 16) / lineH) : 4;
                const int visible = std::min((int)q.size(), maxLines);
                if (visible <= 0) return;
                ScrollState &st = g_nodeScroll[nodeId];
                if (dir < 0) st.sel = (st.sel <= 0) ? (visible - 1) : (st.sel - 1);
                else         st.sel = (st.sel + 1) % visible;
                st.offset = 0; st.lastMs = millis();
                setFastFramerate(); forceDisplay();
            };
            auto moveSelCH = [&](uint8_t ch, int dir) {
                const auto &q = chat::ChatHistoryStore::instance().getCHAN(ch);
                const int h = dispdev->getHeight();
                const int lineH = 10;
                const int maxLines = ((h - 16) / lineH > 4) ? ((h - 16) / lineH) : 4;
                const int visible = std::min((int)q.size(), maxLines);
                if (visible <= 0) return;
                ScrollState &st = g_chanScroll[ch];
                if (dir < 0) st.sel = (st.sel <= 0) ? (visible - 1) : (st.sel - 1);
                else         st.sel = (st.sel + 1) % visible;
                st.offset = 0; st.lastMs = millis();
                setFastFramerate(); forceDisplay();
            };

            // ——— Scroll por pulsación corta en la PANTALLA DE CHAT ———
            const bool shortPressAsDown =
                g_chatScrollByPress &&
                (inNodeChat || inChanChat) &&
                (event->inputEvent == INPUT_BROKER_USER_PRESS || event->inputEvent == INPUT_BROKER_SELECT);

                        if (inNodeChat || inChanChat) {
    // --- mover selección con UP/DOWN ---
    if (event->inputEvent == INPUT_BROKER_UP) {
        if (inNodeChat) {
            uint32_t nodeId = g_favChatNodes[(size_t)cf - g_favChatFirst];
            moveSelDM(nodeId, -1);
        } else {
            uint8_t ch = g_chanTabs[(size_t)cf - g_chanTabFirst];
            moveSelCH(ch, -1);
        }
        return 1;
    }

    if (event->inputEvent == INPUT_BROKER_DOWN) {
        if (inNodeChat) {
            uint32_t nodeId = g_favChatNodes[(size_t)cf - g_favChatFirst];
            moveSelDM(nodeId, +1);
        } else {
            uint8_t ch = g_chanTabs[(size_t)cf - g_chanTabFirst];
            moveSelCH(ch, +1);
        }
        return 1;
    }

    // --- scroll por PULSACIÓN CORTA (sólo si está activado) ---
    if (g_chatScrollByPress && event->inputEvent == INPUT_BROKER_USER_PRESS) {
        if (inNodeChat) {
            uint32_t nodeId = g_favChatNodes[(size_t)cf - g_favChatFirst];
            moveSelDM(nodeId, +1);
        } else {
            uint8_t ch = g_chanTabs[(size_t)cf - g_chanTabFirst];
            moveSelCH(ch, +1);
        }
        return 1;
    }

    // --- abrir menú de chat con SELECT o SELECT_LONG (siempre) ---
    if (event->inputEvent == INPUT_BROKER_SELECT ||
        event->inputEvent == INPUT_BROKER_SELECT_LONG) {
        if (inNodeChat) {
            size_t idx = (size_t)cf - g_favChatFirst;
            if (idx < g_favChatNodes.size()) openChatActionsForNode(g_favChatNodes[idx]);
        } else {
            size_t idx = (size_t)cf - g_chanTabFirst;
            if (idx < g_chanTabs.size()) openChatActionsForChannel(g_chanTabs[idx]);
        }
        return 1;
    }
}





            // === COMPORTAMIENTO GLOBAL: ARRIBA/ABAJO = navegar frames ===
            if (event->inputEvent == INPUT_BROKER_UP) {
                showPrevFrame();
                return 1;
            } else if (event->inputEvent == INPUT_BROKER_DOWN) {
                showNextFrame();
                return 1;
            }

            // === Navegación global original ===
            if (event->inputEvent == INPUT_BROKER_LEFT || event->inputEvent == INPUT_BROKER_ALT_PRESS) {
                showPrevFrame();
            } else if (event->inputEvent == INPUT_BROKER_RIGHT || event->inputEvent == INPUT_BROKER_USER_PRESS) {
                showNextFrame();
            } else if (event->inputEvent == INPUT_BROKER_SELECT) {
                uint8_t cff = this->ui->getUiState()->currentFrame;

                if (cff == framesetInfo.positions.home) {
                    menuHandler::homeBaseMenu();
                } else if (cff == framesetInfo.positions.system) {
                    menuHandler::systemBaseMenu();
#if HAS_GPS
                } else if (cff == framesetInfo.positions.gps && gps) {
                    menuHandler::positionBaseMenu();
#endif
                } else if (cff == framesetInfo.positions.clock) {
                    menuHandler::clockMenu();
                } else if (cff == framesetInfo.positions.lora) {
                    menuHandler::loraMenu();
                } else if (cff == framesetInfo.positions.textMessage) {
                    if (devicestate.rx_text_message.from) {
                        menuHandler::messageResponseMenu();
                    } else {
                        menuHandler::textMessageBaseMenu();
                    }
                } else if (framesetInfo.positions.firstFavorite != 255 &&
                           cff >= framesetInfo.positions.firstFavorite &&
                           cff <= framesetInfo.positions.lastFavorite) {
                    menuHandler::favoriteBaseMenu();
                } else if (cff == framesetInfo.positions.nodelist ||
                           cff == framesetInfo.positions.nodelist_lastheard ||
                           cff == framesetInfo.positions.nodelist_hopsignal ||
                           cff == framesetInfo.positions.nodelist_distance ||
                           cff == framesetInfo.positions.nodelist_hopsignal ||
                           cff == framesetInfo.positions.nodelist_bearings) {
                    menuHandler::nodeListMenu();
                } else if (cff == framesetInfo.positions.wifi) {
                    menuHandler::wifiBaseMenu();
                }
            } else if (event->inputEvent == INPUT_BROKER_BACK) {
                showPrevFrame();
            } else if (event->inputEvent == INPUT_BROKER_CANCEL) {
                setOn(false);
            }
        }
    }

    return 0;
}

int Screen::handleAdminMessage(AdminModule_ObserverData *arg)
{
    switch (arg->request->which_payload_variant) {
    case meshtastic_AdminMessage_remove_by_nodenum_tag:
        setFrames(FOCUS_PRESERVE);
        *arg->result = AdminMessageHandleResult::HANDLED;
        break;
    default:
        break;
    }
    return 0;
}

bool Screen::isOverlayBannerShowing()
{
    return NotificationRenderer::isOverlayBannerShowing();
}

} // namespace graphics

#else
graphics::Screen::Screen(ScanI2C::DeviceAddress, meshtastic_Config_DisplayConfig_OledType, OLEDDISPLAY_GEOMETRY) {}
#endif // HAS_SCREEN

bool shouldWakeOnReceivedMessage()
{
    /*
    Do NOT wake screen on message received when:
    - External notifications enabled
    - Role is not client / client_mute
    - Battery level very low
    */
    if (moduleConfig.external_notification.enabled) {
        return false;
    }
    if (!meshtastic_Config_DeviceConfig_Role_CLIENT && !meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE) {
        return false;
    }
    if (powerStatus && powerStatus->getBatteryChargePercent() < 10) {
        return false;
    }
    return true;
}
