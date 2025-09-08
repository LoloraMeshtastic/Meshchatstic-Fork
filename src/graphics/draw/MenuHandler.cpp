#include "configuration.h"
#if HAS_SCREEN

#include "ClockRenderer.h"
#include "GPS.h"
#include "MenuHandler.h"
#include "MeshRadio.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "buzz.h"
#include "graphics/Screen.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/UIRenderer.h"
#include "input/RotaryEncoderInterruptImpl1.h"
#include "input/UpDownInterruptImpl1.h"
#include "main.h"
#include "mesh/MeshTypes.h"
#include "modules/AdminModule.h"
#include "modules/CannedMessageModule.h"
#include "modules/KeyVerificationModule.h"
#include "modules/TraceRouteModule.h"
#include "NotificationRenderer.h"

#if HAS_WIFI && !defined(ARCH_PORTDUINO)
#include <WiFi.h>
#endif

#include <functional>
#include <algorithm>
#include <vector>

extern bool kb_found;
extern CannedMessageModule *cannedMessageModule;

// Toggle global para “scroll por pulsación corta” en pantallas de chat
bool g_chatScrollByPress = false;


extern uint16_t TFT_MESH;

namespace graphics
{
	
// ——— Scroll de chat por pulsación corta (no persistente) ———
bool g_chatScrollByPress = false;


menuHandler::screenMenus menuHandler::menuQueue = menu_none;
bool test_enabled = false;
uint8_t test_count = 0;

// SSID pendiente cuando se requiere password
static String s_wifiPendingSSID;

/* ========================= LORA MENU ========================= */

void graphics::menuHandler::loraMenu()
{
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
    enum { kRegion = 0, kWifi = 1, kBack = 2, kCount = 3 };
    static const char *options[kCount]     = {"Region Picker", "WiFi Config", "Back"};
    static int         optionsEnum[kCount] = {kRegion, kWifi, kBack};
#else
    enum { kRegion = 0, kBack = 1, kCount = 2 };
    static const char *options[kCount]     = {"Region Picker", "Back"};
    static int         optionsEnum[kCount] = {kRegion, kBack};
#endif

    BannerOverlayOptions o;
    o.message         = "LoRa Actions";
    o.durationMs      = 0;
    o.optionsArrayPtr = options;
    o.optionsEnumPtr  = optionsEnum;
    o.optionsCount    = kCount;

    o.bannerCallback  = [](int sel) {
        switch (sel) {
        case kRegion:
            graphics::menuHandler::LoraRegionPicker();
            break;
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
        case kWifi:
            // Cerrar este banner y lanzar el menú WiFi en el loop
            NotificationRenderer::pauseBanner          = true;
            NotificationRenderer::alertBannerUntil     = 1;
            NotificationRenderer::optionsArrayPtr      = nullptr;
            NotificationRenderer::optionsEnumPtr       = nullptr;
            NotificationRenderer::alertBannerOptions   = 0;
            graphics::menuHandler::menuQueue           = graphics::menuHandler::wifi_config_menu;
            if (screen) screen->forceDisplay(true);
            return;
#endif
        case kBack:
        default:
            break;
        }
        if (screen) screen->forceDisplay(true);
    };

    screen->showOverlayBanner(o);
}

/* ========================= ONBOARD / REGION PICKERS ========================= */

void menuHandler::OnboardMessage()
{
    static const char *optionsArray[] = {"OK", "Got it!"};
    enum optionsNumbers { OK, got };
    BannerOverlayOptions bannerOptions;
#if HAS_TFT
    bannerOptions.message = "Welcome to Meshtastic!\nSwipe to navigate and\nlong press to select\nor open a menu.";
#elif defined(BUTTON_PIN)
    bannerOptions.message = "Welcome to Meshtastic!\nClick to navigate and\nlong press to select\nor open a menu.";
#else
    bannerOptions.message = "Welcome to Meshtastic!\nUse the Select button\nto open menus\nand make selections.";
#endif
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 2;
    bannerOptions.bannerCallback  = [](int) {
        menuHandler::menuQueue = menuHandler::no_timeout_lora_picker;
        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::LoraRegionPicker(uint32_t duration)
{
    static const char *optionsArray[] = {
        "Back",    "US",     "EU_433", "EU_868", "CN",     "JP",     "ANZ",   "KR",   "TW",
        "RU",      "IN",     "NZ_865", "TH",     "LORA_24","UA_433", "UA_868","MY_433","MY_919",
        "SG_923",  "PH_433", "PH_868", "PH_915", "ANZ_433","KZ_433", "KZ_863","NP_865","BR_902"
    };
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Set the LoRa region";
    bannerOptions.durationMs      = duration;
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 27;
    bannerOptions.InitialSelected = 0;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected != 0 && config.lora.region != _meshtastic_Config_LoRaConfig_RegionCode(selected)) {
            config.lora.region = _meshtastic_Config_LoRaConfig_RegionCode(selected);
            if (!owner.is_licensed) {
                bool keygenSuccess = false;
                if (config.security.private_key.size == 32) {
                    if (crypto->regeneratePublicKey(config.security.public_key.bytes, config.security.private_key.bytes)) {
                        keygenSuccess = true;
                    }
                } else {
                    LOG_INFO("Generate new PKI keys");
                    crypto->generateKeyPair(config.security.public_key.bytes, config.security.private_key.bytes);
                    keygenSuccess = true;
                }
                if (keygenSuccess) {
                    config.security.public_key.size  = 32;
                    config.security.private_key.size = 32;
                    owner.public_key.size            = 32;
                    memcpy(owner.public_key.bytes, config.security.public_key.bytes, 32);
                }
            }
            config.lora.tx_enabled = true;
            initRegion();
            if (myRegion->dutyCycle < 100) {
                config.lora.ignore_mqtt = true;
            }
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = (millis() + DEFAULT_REBOOT_SECONDS * 1000);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

/* ========================= CLOCK / TIME ========================= */

void menuHandler::TwelveHourPicker()
{
    static const char *optionsArray[] = {"Back", "12-hour", "24-hour"};
    enum optionsNumbers { Back = 0, twelve = 1, twentyfour = 2 };
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Time Format";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 3;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Back) {
            menuHandler::menuQueue = menuHandler::clock_menu;
            screen->runNow();
        } else if (selected == twelve) {
            config.display.use_12h_clock = true;
        } else {
            config.display.use_12h_clock = false;
        }
        service->reloadConfig(SEGMENT_CONFIG);
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::showConfirmationBanner(const char *message, std::function<void()> onConfirm)
{
    static const char *confirmOptions[] = {"No", "Yes"};
    BannerOverlayOptions confirmBanner;
    confirmBanner.message         = message;
    confirmBanner.optionsArrayPtr = confirmOptions;
    confirmBanner.optionsCount    = 2;
    confirmBanner.bannerCallback  = [onConfirm](int confirmSelected) {
        if (confirmSelected == 1) onConfirm();
    };
    screen->showOverlayBanner(confirmBanner);
}

void menuHandler::ClockFacePicker()
{
    static const char *optionsArray[] = {"Back", "Digital", "Analog"};
    enum optionsNumbers { Back = 0, Digital = 1, Analog = 2 };
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Which Face?";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 3;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Back) {
            menuHandler::menuQueue = menuHandler::clock_menu;
            screen->runNow();
        } else if (selected == Digital) {
            uiconfig.is_clockface_analog = false;
            saveUIConfig();
            screen->setFrames(Screen::FOCUS_CLOCK);
        } else {
            uiconfig.is_clockface_analog = true;
            saveUIConfig();
            screen->setFrames(Screen::FOCUS_CLOCK);
        }
    };
    bannerOptions.InitialSelected = uiconfig.is_clockface_analog ? 2 : 1;
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::TZPicker()
{
    static const char *optionsArray[] = {
        "Back", "US/Hawaii", "US/Alaska", "US/Pacific", "US/Arizona", "US/Mountain", "US/Central", "US/Eastern",
        "BR/Brasilia", "UTC", "EU/Western", "EU/Central", "EU/Eastern", "Asia/Kolkata", "Asia/Hong_Kong",
        "AU/AWST", "AU/ACST", "AU/AEST", "Pacific/NZ"
    };
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Pick Timezone";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 19;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 0) {
            menuHandler::menuQueue = menuHandler::clock_menu;
            screen->runNow();
        } else if (selected == 1) {
            strncpy(config.device.tzdef, "HST10", sizeof(config.device.tzdef));
        } else if (selected == 2) {
            strncpy(config.device.tzdef, "AKST9AKDT,M3.2.0,M11.1.0", sizeof(config.device.tzdef));
        } else if (selected == 3) {
            strncpy(config.device.tzdef, "PST8PDT,M3.2.0,M11.1.0", sizeof(config.device.tzdef));
        } else if (selected == 4) {
            strncpy(config.device.tzdef, "MST7", sizeof(config.device.tzdef));
        } else if (selected == 5) {
            strncpy(config.device.tzdef, "MST7MDT,M3.2.0,M11.1.0", sizeof(config.device.tzdef));
        } else if (selected == 6) {
            strncpy(config.device.tzdef, "CST6CDT,M3.2.0,M11.1.0", sizeof(config.device.tzdef));
        } else if (selected == 7) {
            strncpy(config.device.tzdef, "EST5EDT,M3.2.0,M11.1.0", sizeof(config.device.tzdef));
        } else if (selected == 8) {
            strncpy(config.device.tzdef, "BRT3", sizeof(config.device.tzdef));
        } else if (selected == 9) {
            strncpy(config.device.tzdef, "UTC0", sizeof(config.device.tzdef));
        } else if (selected == 10) {
            strncpy(config.device.tzdef, "GMT0BST,M3.5.0/1,M10.5.0", sizeof(config.device.tzdef));
        } else if (selected == 11) {
            strncpy(config.device.tzdef, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(config.device.tzdef));
        } else if (selected == 12) {
            strncpy(config.device.tzdef, "EET-2EEST,M3.5.0/3,M10.5.0/4", sizeof(config.device.tzdef));
        } else if (selected == 13) {
            strncpy(config.device.tzdef, "IST-5:30", sizeof(config.device.tzdef));
        } else if (selected == 14) {
            strncpy(config.device.tzdef, "HKT-8", sizeof(config.device.tzdef));
        } else if (selected == 15) {
            strncpy(config.device.tzdef, "AWST-8", sizeof(config.device.tzdef));
        } else if (selected == 16) {
            strncpy(config.device.tzdef, "ACST-9:30ACDT,M10.1.0,M4.1.0/3", sizeof(config.device.tzdef));
        } else if (selected == 17) {
            strncpy(config.device.tzdef, "AEST-10AEDT,M10.1.0,M4.1.0/3", sizeof(config.device.tzdef));
        } else if (selected == 18) {
            strncpy(config.device.tzdef, "NZST-12NZDT,M9.5.0,M4.1.0/3", sizeof(config.device.tzdef));
        }
        if (selected != 0) {
            setenv("TZ", config.device.tzdef, 1);
            service->reloadConfig(SEGMENT_CONFIG);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::clockMenu()
{
    static const char *optionsArray[] = {"Back", "Clock Face", "Time Format", "Timezone"};
    enum optionsNumbers { Back = 0, Clock = 1, Time = 2, Timezone = 3 };
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Clock Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 4;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Clock) {
            menuHandler::menuQueue = menuHandler::clock_face_picker;
            screen->runNow();
        } else if (selected == Time) {
            menuHandler::menuQueue = menuHandler::twelve_hour_picker;
            screen->runNow();
        } else if (selected == Timezone) {
            menuHandler::menuQueue = menuHandler::TZ_picker;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

/* ========================= MESSAGE MENUS ========================= */

void menuHandler::messageResponseMenu()
{
    enum optionsNumbers { Back = 0, Dismiss = 1, Preset = 2, Freetext = 3, Aloud = 4, enumEnd = 5 };

    static const char *optionsArray[enumEnd] = {"Back", "Dismiss", "Reply via Preset"};
    static int optionsEnumArray[enumEnd]     = {Back, Dismiss, Preset};
    int options                               = 3;

    if (kb_found) {
        optionsArray[options]    = "Reply via Freetext";
        optionsEnumArray[options++] = Freetext;
    }

#ifdef HAS_I2S
    optionsArray[options]        = "Read Aloud";
    optionsEnumArray[options++]  = Aloud;
#endif

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Message Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.optionsCount    = options;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Dismiss) {
            screen->hideCurrentFrame();
        } else if (selected == Preset) {
            if (devicestate.rx_text_message.to == NODENUM_BROADCAST) {
                cannedMessageModule->LaunchWithDestination(NODENUM_BROADCAST, devicestate.rx_text_message.channel);
            } else {
                cannedMessageModule->LaunchWithDestination(devicestate.rx_text_message.from);
            }
        } else if (selected == Freetext) {
            if (devicestate.rx_text_message.to == NODENUM_BROADCAST) {
                cannedMessageModule->LaunchFreetextWithDestination(NODENUM_BROADCAST, devicestate.rx_text_message.channel);
            } else {
                cannedMessageModule->LaunchFreetextWithDestination(devicestate.rx_text_message.from);
            }
        }
#ifdef HAS_I2S
        else if (selected == Aloud) {
            const meshtastic_MeshPacket &mp = devicestate.rx_text_message;
            const char *msg = reinterpret_cast<const char *>(mp.decoded.payload.bytes);
            audioThread->readAloud(msg);
        }
#endif
    };
    screen->showOverlayBanner(bannerOptions);
}

/* ========================= HOME / TEXT MENUS ========================= */

void menuHandler::homeBaseMenu()
{
    enum optionsNumbers { Back, Backlight, Position, Preset, Freetext, Sleep, enumEnd };

    static const char *optionsArray[enumEnd] = {"Back"};
    static int optionsEnumArray[enumEnd]     = {Back};
    int options                               = 1;

#if defined(PIN_EINK_EN) || defined(PCA_PIN_EINK_EN)
    optionsArray[options]       = "Toggle Backlight";
    optionsEnumArray[options++] = Backlight;
#else
    optionsArray[options]       = "Sleep Screen";
    optionsEnumArray[options++] = Sleep;
#endif

    if (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
        optionsArray[options] = "Send Position";
    } else {
        optionsArray[options] = "Send Node Info";
    }
    optionsEnumArray[options++] = Position;

    if (!kb_found) {
        optionsArray[options]       = "New Preset Msg";
        optionsEnumArray[options++] = Preset;
    }
    if (kb_found) {
        optionsArray[options]       = "New Freetext Msg";
        optionsEnumArray[options++] = Freetext;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Home Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.optionsCount    = options;

    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Backlight) {
#if defined(PIN_EINK_EN)
            if (uiconfig.screen_brightness == 1) {
                uiconfig.screen_brightness = 0;
                digitalWrite(PIN_EINK_EN, LOW);
            } else {
                uiconfig.screen_brightness = 1;
                digitalWrite(PIN_EINK_EN, HIGH);
            }
            saveUIConfig();
#elif defined(PCA_PIN_EINK_EN)
            if (uiconfig.screen_brightness == 1) {
                uiconfig.screen_brightness = 0;
                io.digitalWrite(PCA_PIN_EINK_EN, LOW);
            } else {
                uiconfig.screen_brightness = 1;
                io.digitalWrite(PCA_PIN_EINK_EN, HIGH);
            }
            saveUIConfig();
#endif
        } else if (selected == Sleep) {
            screen->setOn(false);
        } else if (selected == Position) {
            InputEvent event = {.inputEvent = (input_broker_event)INPUT_BROKER_SEND_PING, .kbchar = 0, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
        } else if (selected == Preset) {
            cannedMessageModule->LaunchWithDestination(NODENUM_BROADCAST);
        } else if (selected == Freetext) {
            cannedMessageModule->LaunchFreetextWithDestination(NODENUM_BROADCAST);
        }
    };

    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::textMessageBaseMenu()
{
    enum optionsNumbers { Back, Preset, Freetext, enumEnd };

    static const char *optionsArray[enumEnd] = {"Back"};
    static int optionsEnumArray[enumEnd]     = {Back};
    int options                               = 1;

    optionsArray[options]       = "New Preset Msg";
    optionsEnumArray[options++] = Preset;
    if (kb_found) {
        optionsArray[options]       = "New Freetext Msg";
        optionsEnumArray[options++] = Freetext;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Message Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.optionsCount    = options;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Preset) {
            cannedMessageModule->LaunchWithDestination(NODENUM_BROADCAST);
        } else if (selected == Freetext) {
            cannedMessageModule->LaunchFreetextWithDestination(NODENUM_BROADCAST);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

/* ========================= SYSTEM MENUS ========================= */

void menuHandler::systemBaseMenu()
{
    enum optionsNumbers { Back, Notifications, ScreenOptions, Bluetooth, PowerMenu, FrameToggles, Test, enumEnd };
    static const char *optionsArray[enumEnd] = {"Back"};
    static int optionsEnumArray[enumEnd]     = {Back};
    int options                               = 1;

    optionsArray[options]       = "Notifications";
    optionsEnumArray[options++] = Notifications;
#if defined(ST7789_CS) || defined(ST7796_CS) || defined(USE_OLED) || defined(USE_SSD1306) || defined(USE_SH1106) || \
    defined(USE_SH1107) || defined(HELTEC_MESH_NODE_T114) || defined(HELTEC_VISION_MASTER_T190) || HAS_TFT
    optionsArray[options]       = "Screen Options";
    optionsEnumArray[options++] = ScreenOptions;
#endif

    optionsArray[options]       = "Frame Visiblity Toggle";
    optionsEnumArray[options++] = FrameToggles;

    optionsArray[options]       = "Bluetooth Toggle";
    optionsEnumArray[options++] = Bluetooth;

    optionsArray[options]       = "Reboot/Shutdown";
    optionsEnumArray[options++] = PowerMenu;

    if (test_enabled) {
        optionsArray[options]       = "Test Menu";
        optionsEnumArray[options++] = Test;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "System Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = options;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Notifications) {
            menuHandler::menuQueue = menuHandler::notifications_menu;
            screen->runNow();
        } else if (selected == ScreenOptions) {
            menuHandler::menuQueue = menuHandler::screen_options_menu;
            screen->runNow();
        } else if (selected == PowerMenu) {
            menuHandler::menuQueue = menuHandler::power_menu;
            screen->runNow();
        } else if (selected == FrameToggles) {
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == Test) {
            menuHandler::menuQueue = menuHandler::test_menu;
            screen->runNow();
        } else if (selected == Bluetooth) {
            menuQueue = bluetooth_toggle_menu;
            screen->runNow();
        } else if (selected == Back && !test_enabled) {
            test_count++;
            if (test_count > 4) test_enabled = true;
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::favoriteBaseMenu()
{
    enum optionsNumbers { Back, Preset, Freetext, Remove, TraceRoute, enumEnd };
    static const char *optionsArray[enumEnd] = {"Back", "New Preset Msg"};
    static int optionsEnumArray[enumEnd]     = {Back, Preset};
    int options                               = 2;

    if (kb_found) {
        optionsArray[options]       = "New Freetext Msg";
        optionsEnumArray[options++] = Freetext;
    }
    optionsArray[options]       = "Trace Route";
    optionsEnumArray[options++] = TraceRoute;
    optionsArray[options]       = "Remove Favorite";
    optionsEnumArray[options++] = Remove;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Favorites Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.optionsCount    = options;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Preset) {
            cannedMessageModule->LaunchWithDestination(graphics::UIRenderer::currentFavoriteNodeNum);
        } else if (selected == Freetext) {
            cannedMessageModule->LaunchFreetextWithDestination(graphics::UIRenderer::currentFavoriteNodeNum);
        } else if (selected == Remove) {
            menuHandler::menuQueue = menuHandler::remove_favorite;
            screen->runNow();
        } else if (selected == TraceRoute) {
            if (traceRouteModule) {
                traceRouteModule->launch(graphics::UIRenderer::currentFavoriteNodeNum);
            }
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::positionBaseMenu()
{
    enum optionsNumbers { Back, GPSToggle, CompassMenu, CompassCalibrate, enumEnd };

    static const char *optionsArray[enumEnd] = {"Back", "GPS Toggle", "Compass"};
    static int optionsEnumArray[enumEnd]     = {Back, GPSToggle, CompassMenu};
    int options                               = 3;

    if (accelerometerThread) {
        optionsArray[options]       = "Compass Calibrate";
        optionsEnumArray[options++] = CompassCalibrate;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Position Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.optionsCount    = options;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == GPSToggle) {
            menuQueue = gps_toggle_menu;
            screen->runNow();
        } else if (selected == CompassMenu) {
            menuQueue = compass_point_north_menu;
            screen->runNow();
        } else if (selected == CompassCalibrate) {
            accelerometerThread->calibrate(30);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::nodeListMenu()
{
    enum optionsNumbers { Back, Favorite, TraceRoute, Verify, Reset, enumEnd };
    static const char *optionsArray[] = {"Back", "Add Favorite", "Trace Route", "Key Verification", "Reset NodeDB"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Node Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 5;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Favorite) {
            menuQueue = add_favorite;
            screen->runNow();
        } else if (selected == Verify) {
            menuQueue = key_verification_init;
            screen->runNow();
        } else if (selected == Reset) {
            menuQueue = reset_node_db_menu;
            screen->runNow();
        } else if (selected == TraceRoute) {
            menuQueue = trace_route_menu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::resetNodeDBMenu()
{
    static const char *optionsArray[] = {"Back", "Confirm"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Confirm Reset NodeDB";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 2;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 1) {
            disableBluetooth();
            LOG_INFO("Initiate node-db reset");
            nodeDB->resetNodes();
            rebootAtMsec = (millis() + DEFAULT_REBOOT_SECONDS * 1000);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::compassNorthMenu()
{
    enum optionsNumbers { Back, Dynamic, Fixed, Freeze };
    static const char *optionsArray[] = {"Back", "Dynamic", "Fixed Ring", "Freeze Heading"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "North Directions?";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 4;
    bannerOptions.InitialSelected = uiconfig.compass_mode + 1;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Dynamic) {
            if (uiconfig.compass_mode != meshtastic_CompassMode_DYNAMIC) {
                uiconfig.compass_mode = meshtastic_CompassMode_DYNAMIC;
                saveUIConfig();
                screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
            }
        } else if (selected == Fixed) {
            if (uiconfig.compass_mode != meshtastic_CompassMode_FIXED_RING) {
                uiconfig.compass_mode = meshtastic_CompassMode_FIXED_RING;
                saveUIConfig();
                screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
            }
        } else if (selected == Freeze) {
            if (uiconfig.compass_mode != meshtastic_CompassMode_FREEZE_HEADING) {
                uiconfig.compass_mode = meshtastic_CompassMode_FREEZE_HEADING;
                saveUIConfig();
                screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
            }
        } else if (selected == Back) {
            menuQueue = position_base_menu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

#if !MESHTASTIC_EXCLUDE_GPS
void menuHandler::GPSToggleMenu()
{
    static const char *optionsArray[] = {"Back", "Enabled", "Disabled"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Toggle GPS";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 3;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 1) {
            config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;
            playGPSEnableBeep();
            gps->enable();
            service->reloadConfig(SEGMENT_CONFIG);
        } else if (selected == 2) {
            config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_DISABLED;
            playGPSDisableBeep();
            gps->disable();
            service->reloadConfig(SEGMENT_CONFIG);
        } else {
            menuQueue = position_base_menu;
            screen->runNow();
        }
    };
    bannerOptions.InitialSelected = (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) ? 1 : 2;
    screen->showOverlayBanner(bannerOptions);
}
#endif

void menuHandler::BluetoothToggleMenu()
{
    static const char *optionsArray[] = {"Back", "Enabled", "Disabled"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Toggle Bluetooth";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 3;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 1 || selected == 2) {
            InputEvent event = {.inputEvent = (input_broker_event)170, .kbchar = 170, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
        }
    };
    bannerOptions.InitialSelected = config.bluetooth.enabled ? 1 : 2;
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::BuzzerModeMenu()
{
    static const char *optionsArray[] = {"All Enabled", "Disabled", "Notifications", "System Only"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Buzzer Mode";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 4;
    bannerOptions.bannerCallback  = [](int selected) {
        config.device.buzzer_mode = (meshtastic_Config_DeviceConfig_BuzzerMode)selected;
        service->reloadConfig(SEGMENT_CONFIG);
    };
    bannerOptions.InitialSelected = config.device.buzzer_mode;
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::BrightnessPickerMenu()
{
    static const char *optionsArray[] = {"Back", "Low", "Medium", "High"};

    int currentSelection = 1;
    if (uiconfig.screen_brightness >= 255) currentSelection = 3;
    else if (uiconfig.screen_brightness >= 128) currentSelection = 2;
    else currentSelection = 1;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Brightness";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 4;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 1) {
            uiconfig.screen_brightness = 64;
        } else if (selected == 2) {
            uiconfig.screen_brightness = 128;
        } else if (selected == 3) {
            uiconfig.screen_brightness = 255;
        }

        if (selected != 0) {
#if defined(HELTEC_MESH_NODE_T114) || defined(HELTEC_VISION_MASTER_T190)
            analogWrite(VTFT_LEDA, uiconfig.screen_brightness);
#elif defined(ST7789_CS) || defined(ST7796_CS)
            static_cast<TFTDisplay *>(screen->getDisplayDevice())->setDisplayBrightness(uiconfig.screen_brightness);
#elif defined(USE_OLED) || defined(USE_SSD1306) || defined(USE_SH1106) || defined(USE_SH1107)
            screen->getDisplayDevice()->setBrightness(uiconfig.screen_brightness);
#endif
            saveUIConfig();
            LOG_INFO("Screen brightness set to %d", uiconfig.screen_brightness);
        }
    };
    bannerOptions.InitialSelected = currentSelection;
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::switchToMUIMenu()
{
    static const char *optionsArray[] = {"No", "Yes"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Switch to MUI?";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 2;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 1) {
            config.display.displaymode = meshtastic_Config_DisplayConfig_DisplayMode_COLOR;
            config.bluetooth.enabled   = false;
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = (millis() + DEFAULT_REBOOT_SECONDS * 1000);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::TFTColorPickerMenu(OLEDDisplay *display)
{
    static const char *optionsArray[] = {"Back", "Default", "Meshtastic Green", "Yellow", "Red", "Orange", "Purple", "Teal",
                                         "Pink", "White"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Select Screen Color";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 10;
    bannerOptions.bannerCallback  = [display](int selected) {
#if defined(HELTEC_MESH_NODE_T114) || defined(HELTEC_VISION_MASTER_T190) || defined(T_DECK) || defined(T_LORA_PAGER) || HAS_TFT
        uint8_t TFT_MESH_r = 0, TFT_MESH_g = 0, TFT_MESH_b = 0;
        if (selected == 1) {
            // Default (leer de override si existe)
        } else if (selected == 2) {
            TFT_MESH_r = 103; TFT_MESH_g = 234; TFT_MESH_b = 148;
        } else if (selected == 3) {
            TFT_MESH_r = 255; TFT_MESH_g = 255; TFT_MESH_b = 128;
        } else if (selected == 4) {
            TFT_MESH_r = 255; TFT_MESH_g = 64;  TFT_MESH_b = 64;
        } else if (selected == 5) {
            TFT_MESH_r = 255; TFT_MESH_g = 160; TFT_MESH_b = 20;
        } else if (selected == 6) {
            TFT_MESH_r = 204; TFT_MESH_g = 153; TFT_MESH_b = 255;
        } else if (selected == 7) {
            TFT_MESH_r = 64;  TFT_MESH_g = 224; TFT_MESH_b = 208;
        } else if (selected == 8) {
            TFT_MESH_r = 255; TFT_MESH_g = 105; TFT_MESH_b = 180;
        } else if (selected == 9) {
            TFT_MESH_r = 255; TFT_MESH_g = 255; TFT_MESH_b = 255;
        } else {
            menuQueue = system_base_menu;
            screen->runNow();
        }

        if (selected != 0) {
            display->setColor(BLACK);
            display->fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
            display->setColor(WHITE);

            if (TFT_MESH_r == 0 && TFT_MESH_g == 0 && TFT_MESH_b == 0) {
#ifdef TFT_MESH_OVERRIDE
                TFT_MESH = TFT_MESH_OVERRIDE;
#else
                TFT_MESH = COLOR565(0x67, 0xEA, 0x94);
#endif
            } else {
                TFT_MESH = COLOR565(TFT_MESH_r, TFT_MESH_g, TFT_MESH_b);
            }

#if defined(HELTEC_MESH_NODE_T114) || defined(HELTEC_VISION_MASTER_T190)
            static_cast<ST7789Spi *>(screen->getDisplayDevice())->setRGB(TFT_MESH);
#endif

            screen->setFrames(graphics::Screen::FOCUS_SYSTEM);
            if (TFT_MESH_r == 0 && TFT_MESH_g == 0 && TFT_MESH_b == 0) {
                uiconfig.screen_rgb_color = 0;
            } else {
                uiconfig.screen_rgb_color = (TFT_MESH_r << 16) | (TFT_MESH_g << 8) | TFT_MESH_b;
            }
            LOG_INFO("Storing Value of %d to uiconfig.screen_rgb_color", uiconfig.screen_rgb_color);
            saveUIConfig();
        }
#endif
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::rebootMenu()
{
    static const char *optionsArray[] = {"Back", "Confirm"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Reboot Device?";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 2;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 1) {
            IF_SCREEN(screen->showSimpleBanner("Rebooting...", 0));
            nodeDB->saveToDisk();
            rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
        } else {
            menuQueue = power_menu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::shutdownMenu()
{
    static const char *optionsArray[] = {"Back", "Confirm"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Shutdown Device?";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 2;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 1) {
            InputEvent event = {.inputEvent = (input_broker_event)INPUT_BROKER_SHUTDOWN, .kbchar = 0, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
        } else {
            menuQueue = power_menu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::addFavoriteMenu()
{
    screen->showNodePicker("Node To Favorite", 30000, [](uint32_t nodenum) {
        LOG_WARN("Nodenum: %u", nodenum);
        nodeDB->set_favorite(true, nodenum);
        screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
    });
}

void menuHandler::removeFavoriteMenu()
{
    static const char *optionsArray[] = {"Back", "Yes"};
    BannerOverlayOptions bannerOptions;
    std::string message = "Unfavorite This Node?\n";
    auto node = nodeDB->getMeshNode(graphics::UIRenderer::currentFavoriteNodeNum);
    if (node && node->has_user) {
        message += sanitizeString(node->user.long_name).substr(0, 15);
    }
    bannerOptions.message         = message.c_str();
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 2;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 1) {
            LOG_INFO("Removing %x as favorite node", graphics::UIRenderer::currentFavoriteNodeNum);
            nodeDB->set_favorite(false, graphics::UIRenderer::currentFavoriteNodeNum);
            screen->setFrames(graphics::Screen::FOCUS_DEFAULT);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::traceRouteMenu()
{
    screen->showNodePicker("Node to Trace", 30000, [](uint32_t nodenum) {
        LOG_INFO("Menu: Node picker selected node 0x%08x, traceRouteModule=%p", nodenum, traceRouteModule);
        if (traceRouteModule) traceRouteModule->startTraceRoute(nodenum);
    });
}

void menuHandler::testMenu()
{
    static const char *optionsArray[] = {"Back", "Number Picker"};
    BannerOverlayOptions bannerOptions;
    std::string message = "Test to Run?\n";
    bannerOptions.message         = message.c_str();
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 2;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == 1) {
            menuQueue = number_test;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::numberTest()
{
    screen->showNumberPicker("Pick a number\n ", 30000, 4, [](int number_picked) {
        LOG_WARN("Nodenum: %u", number_picked);
    });
}

/* ========================= WIFI MENUS ========================= */

void menuHandler::wifiBaseMenu()
{
    enum optionsNumbers { Back, Wifi_toggle };

    static const char *optionsArray[] = {"Back", "WiFi Toggle"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "WiFi Menu";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 2;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Wifi_toggle) {
            menuQueue = wifi_toggle_menu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void graphics::menuHandler::wifiConfigMenu()
{
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
    // Cierra cualquier teclado virtual previo
    if (NotificationRenderer::virtualKeyboard) {
        delete NotificationRenderer::virtualKeyboard;
        NotificationRenderer::virtualKeyboard = nullptr;
    }

    // Escaneo WiFi (bloqueante)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(60);
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);

    struct AP { String ssid; int rssi; bool open; };
    std::vector<AP> aps;
    aps.reserve(n > 0 ? n : 0);

    for (int i = 0; i < n; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        int  rssi  = WiFi.RSSI(i);
        bool open  = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

        auto it = std::find_if(aps.begin(), aps.end(), [&](const AP& a){ return a.ssid == ssid; });
        if (it == aps.end()) aps.push_back({ssid, rssi, open});
        else if (rssi > it->rssi) { it->rssi = rssi; it->open = open; }
    }

    std::sort(aps.begin(), aps.end(), [](const AP& a, const AP& b){ return a.rssi > b.rssi; });

    const int MAX_SHOW = 12;
    static char labels[MAX_SHOW + 3][40];
    static const char* options[MAX_SHOW + 3];

    int count = 0;
    int apShown = (int)std::min(aps.size(), (size_t)MAX_SHOW);
    for (int i = 0; i < apShown; ++i) {
        const auto &ap = aps[i];
        String ss = ap.ssid;
        if (ss.length() > 22) ss = ss.substring(0, 19) + "...";
        snprintf(labels[count], sizeof(labels[count]), "%s", ss.c_str());
        options[count++] = labels[i];
    }

    if (apShown == 0) {
        snprintf(labels[count], sizeof(labels[count]), "No networks");
        options[count++] = labels[count];
    }

    int rescanIdx = count;
    options[count++] = "Rescan";
    int backIdx = count;
    options[count++] = "Back";

    static std::vector<AP> s_aps;
    static int s_apShown, s_rescanIdx, s_backIdx;
    s_aps       = aps;
    s_apShown   = apShown;
    s_rescanIdx = rescanIdx;
    s_backIdx   = backIdx;

    BannerOverlayOptions o;
    o.message         = "WiFi Networks";
    o.durationMs      = 0;
    o.optionsArrayPtr = options;
    o.optionsCount    = count;
    o.optionsEnumPtr  = nullptr; // devolvemos índice

    o.bannerCallback = [](int sel) {
        if (sel == s_rescanIdx) {
            graphics::menuHandler::menuQueue = graphics::menuHandler::wifi_config_menu;
            if (screen) screen->forceDisplay(true);
            return;
        }
        if (sel == s_backIdx) {
            if (screen) screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
            return;
        }

        if (s_apShown == 0) return;
        if (sel < 0 || sel >= s_apShown) return;

        const String ssidSel = s_aps[sel].ssid;
        const bool   open    = s_aps[sel].open;

        if (open) {
            graphics::menuHandler::showConfirmationBanner("Open network. Connect?", [ssidSel]() {
                config.network.wifi_enabled = true;
                strlcpy(config.network.wifi_ssid, ssidSel.c_str(), sizeof(config.network.wifi_ssid));
                config.network.wifi_psk[0] = '\0';
                service->reloadConfig(SEGMENT_CONFIG);

                WiFi.mode(WIFI_STA);
                WiFi.disconnect(true);
                delay(50);
                WiFi.begin(ssidSel.c_str());
                if (screen) screen->showSimpleBanner("Connecting...", 2000);
            });
            return;
        }

        // contraseña requerida
        NotificationRenderer::pauseBanner         = true;
        NotificationRenderer::alertBannerUntil    = 1;
        NotificationRenderer::optionsArrayPtr     = nullptr;
        NotificationRenderer::optionsEnumPtr      = nullptr;
        NotificationRenderer::alertBannerOptions  = 0;

        s_wifiPendingSSID = ssidSel;
        graphics::menuHandler::menuQueue = graphics::menuHandler::wifi_password_prompt;
        if (screen) screen->forceDisplay(true);
    };

    screen->showOverlayBanner(o);
#else
    if (screen) screen->showSimpleBanner("WiFi not available", 2000);
#endif
}

void menuHandler::wifiToggleMenu()
{
    enum optionsNumbers { Back, Wifi_toggle };

    static const char *optionsArray[] = {"Back", "Disable"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Disable Wifi and\nEnable Bluetooth?";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = 2;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Wifi_toggle) {
            config.network.wifi_enabled = false;
            config.bluetooth.enabled    = true;
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = (millis() + DEFAULT_REBOOT_SECONDS * 1000);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

/* ========================= NOTIFICATIONS / SCREEN / POWER ========================= */

void menuHandler::notificationsMenu()
{
    enum optionsNumbers { Back, BuzzerActions };
    static const char *optionsArray[] = {"Back", "Buzzer Actions"};
    static int optionsEnumArray[]     = {Back, BuzzerActions};
    int options                        = 2;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Notifications";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = options;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == BuzzerActions) {
            menuHandler::menuQueue = menuHandler::buzzermodemenupicker;
            screen->runNow();
        } else {
            menuQueue = system_base_menu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::screenOptionsMenu()
{
    bool hasSupportBrightness = false;
#if defined(ST7789_CS) || defined(USE_OLED) || defined(USE_SSD1306) || defined(USE_SH1106) || defined(USE_SH1107)
    hasSupportBrightness = true;
#endif
#if defined(T_DECK)
    hasSupportBrightness = false;
#endif

    enum optionsNumbers { Back, Brightness, ScreenColor };
    static const char *optionsArray[4] = {"Back"};
    static int optionsEnumArray[4]     = {Back};
    int options                         = 1;

    if (hasSupportBrightness) {
        optionsArray[options]       = "Brightness";
        optionsEnumArray[options++] = Brightness;
    }

#if defined(HELTEC_MESH_NODE_T114) || defined(HELTEC_VISION_MASTER_T190) || defined(T_DECK) || defined(T_LORA_PAGER) || HAS_TFT
    optionsArray[options]       = "Screen Color";
    optionsEnumArray[options++] = ScreenColor;
#endif

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Screen Options";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = options;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Brightness) {
            menuHandler::menuQueue = menuHandler::brightness_picker;
            screen->runNow();
        } else if (selected == ScreenColor) {
            menuHandler::menuQueue = menuHandler::tftcolormenupicker;
            screen->runNow();
        } else {
            menuQueue = system_base_menu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::powerMenu()
{
    enum optionsNumbers { Back, Reboot, Shutdown, MUI };
    static const char *optionsArray[4] = {"Back"};
    static int optionsEnumArray[4]     = {Back};
    int options                         = 1;

    optionsArray[options]       = "Reboot";
    optionsEnumArray[options++] = Reboot;

    optionsArray[options]       = "Shutdown";
    optionsEnumArray[options++] = Shutdown;

#if HAS_TFT
    optionsArray[options]       = "Switch to MUI";
    optionsEnumArray[options++] = MUI;
#endif

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Reboot / Shutdown";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = options;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.bannerCallback  = [](int selected) {
        if (selected == Reboot) {
            menuHandler::menuQueue = menuHandler::reboot_menu;
            screen->runNow();
        } else if (selected == Shutdown) {
            menuHandler::menuQueue = menuHandler::shutdown_menu;
            screen->runNow();
        } else if (selected == MUI) {
            menuHandler::menuQueue = menuHandler::mui_picker;
            screen->runNow();
        } else {
            menuQueue = system_base_menu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

/* ========================= KEY VERIFICATION ========================= */

void menuHandler::keyVerificationInitMenu()
{
    screen->showNodePicker("Node to Verify", 30000, [](uint32_t selected) {
        keyVerificationModule->sendInitialRequest(selected);
    });
}

void menuHandler::keyVerificationFinalPrompt()
{
    char message[40] = {0};
    sprintf(message, "Verification: \n");
    keyVerificationModule->generateVerificationCode(message + 15);

    if (screen) {
        static const char *optionsArray[] = {"Reject", "Accept"};
        graphics::BannerOverlayOptions options;
        options.message          = message;
        options.durationMs       = 30000;
        options.optionsArrayPtr  = optionsArray;
        options.optionsCount     = 2;
        options.notificationType = graphics::notificationTypeEnum::selection_picker;
        options.bannerCallback   = [](int selected) {
            if (selected == 1) {
                auto remoteNodePtr = nodeDB->getMeshNode(keyVerificationModule->getCurrentRemoteNode());
                remoteNodePtr->bitfield |= NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_MASK;
            }
        };
        screen->showOverlayBanner(options);
    }
}

/* ========================= FRAME TOGGLES ========================= */

void menuHandler::FrameToggles_menu()
{
    enum optionsNumbers {
        Finish,
        nodelist,
        nodelist_lastheard,
        nodelist_hopsignal,
        nodelist_distance,
        nodelist_bearings,
        gps,
        lora,
        clock,
        show_favorites,
        enumEnd
    };
    static const char *optionsArray[enumEnd] = {"Finish"};
    static int optionsEnumArray[enumEnd]     = {Finish};
    int options                               = 1;

    static int lastSelectedIndex = 0;

#ifndef USE_EINK
    optionsArray[options]       = screen->isFrameHidden("nodelist") ? "Show Node List" : "Hide Node List";
    optionsEnumArray[options++] = nodelist;
#endif
#ifdef USE_EINK
    optionsArray[options]       = screen->isFrameHidden("nodelist_lastheard") ? "Show NL - Last Heard" : "Hide NL - Last Heard";
    optionsEnumArray[options++] = nodelist_lastheard;
    optionsArray[options]       = screen->isFrameHidden("nodelist_hopsignal") ? "Show NL - Hops/Signal" : "Hide NL - Hops/Signal";
    optionsEnumArray[options++] = nodelist_hopsignal;
    optionsArray[options]       = screen->isFrameHidden("nodelist_distance") ? "Show NL - Distance" : "Hide NL - Distance";
    optionsEnumArray[options++] = nodelist_distance;
#endif
#if HAS_GPS
    optionsArray[options]       = screen->isFrameHidden("nodelist_bearings") ? "Show Bearings" : "Hide Bearings";
    optionsEnumArray[options++] = nodelist_bearings;

    optionsArray[options]       = screen->isFrameHidden("gps") ? "Show Position" : "Hide Position";
    optionsEnumArray[options++] = gps;
#endif

    optionsArray[options]       = screen->isFrameHidden("lora") ? "Show LoRa" : "Hide LoRa";
    optionsEnumArray[options++] = lora;

    optionsArray[options]       = screen->isFrameHidden("clock") ? "Show Clock" : "Hide Clock";
    optionsEnumArray[options++] = clock;

    optionsArray[options]       = screen->isFrameHidden("show_favorites") ? "Show Favorites" : "Hide Favorites";
    optionsEnumArray[options++] = show_favorites;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message         = "Show/Hide Frames";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount    = options;
    bannerOptions.optionsEnumPtr  = optionsEnumArray;
    bannerOptions.InitialSelected = lastSelectedIndex;

    bannerOptions.bannerCallback  = [options](int selected) mutable {
        int idx = 0;
        for (; idx < options; ++idx) if (optionsEnumArray[idx] == selected) break;
        lastSelectedIndex = idx;

        if (selected == Finish) {
            screen->setFrames(Screen::FOCUS_DEFAULT);
        } else if (selected == nodelist) {
            screen->toggleFrameVisibility("nodelist");
            menuHandler::menuQueue = menuHandler::FrameToggles; screen->runNow();
        } else if (selected == nodelist_lastheard) {
            screen->toggleFrameVisibility("nodelist_lastheard");
            menuHandler::menuQueue = menuHandler::FrameToggles; screen->runNow();
        } else if (selected == nodelist_hopsignal) {
            screen->toggleFrameVisibility("nodelist_hopsignal");
            menuHandler::menuQueue = menuHandler::FrameToggles; screen->runNow();
        } else if (selected == nodelist_distance) {
            screen->toggleFrameVisibility("nodelist_distance");
            menuHandler::menuQueue = menuHandler::FrameToggles; screen->runNow();
        } else if (selected == nodelist_bearings) {
            screen->toggleFrameVisibility("nodelist_bearings");
            menuHandler::menuQueue = menuHandler::FrameToggles; screen->runNow();
        } else if (selected == gps) {
            screen->toggleFrameVisibility("gps");
            menuHandler::menuQueue = menuHandler::FrameToggles; screen->runNow();
        } else if (selected == lora) {
            screen->toggleFrameVisibility("lora");
            menuHandler::menuQueue = menuHandler::FrameToggles; screen->runNow();
        } else if (selected == clock) {
            screen->toggleFrameVisibility("clock");
            menuHandler::menuQueue = menuHandler::FrameToggles; screen->runNow();
        } else if (selected == show_favorites) {
            screen->toggleFrameVisibility("show_favorites");
            menuHandler::menuQueue = menuHandler::FrameToggles; screen->runNow();
        }
    };

    screen->showOverlayBanner(bannerOptions);
}

/* ========================= SWITCH DISPATCH ========================= */

void menuHandler::handleMenuSwitch(OLEDDisplay *display)
{
    if (menuQueue != menu_none) test_count = 0;

    switch (menuQueue) {
    case menu_none: break;
    case lora_picker: LoraRegionPicker(); break;

    case wifi_config_menu:
        wifiConfigMenu();
        menuQueue = menu_none;
        return;

    case node_info_menu:
        // No-op
        break;

    case wifi_password_prompt: {
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
        char hdr[48];
        snprintf(hdr, sizeof(hdr), "WiFi: %s", s_wifiPendingSSID.c_str());
        screen->showTextInput(hdr, "", 0, [](const std::string &pass) {
            // persistir config
            config.network.wifi_enabled = true; // <<-- si tu struct es 'wifi_enabled', corrígelo aquí
            // NOTA: en muchos árboles Meshtastic es 'wifi_enabled'. Si es tu caso:
            // config.network.wifi_enabled = true;

            strlcpy(config.network.wifi_ssid, s_wifiPendingSSID.c_str(), sizeof(config.network.wifi_ssid));
            strlcpy(config.network.wifi_psk,  pass.c_str(),             sizeof(config.network.wifi_psk));
            service->reloadConfig(SEGMENT_CONFIG);

            // aplicar ahora
            WiFi.mode(WIFI_STA);
            WiFi.disconnect(true);
            delay(50);
            WiFi.begin(s_wifiPendingSSID.c_str(), pass.c_str());
            if (screen) screen->showSimpleBanner("Connecting...", 2000);
        });
#endif
        menuQueue = menu_none;
        return;
    }

    case no_timeout_lora_picker: LoraRegionPicker(0); break;
    case TZ_picker: TZPicker(); break;
    case twelve_hour_picker: TwelveHourPicker(); break;
    case clock_face_picker: ClockFacePicker(); break;
    case clock_menu: clockMenu(); break;
    case system_base_menu: systemBaseMenu(); break;
    case position_base_menu: positionBaseMenu(); break;
#if !MESHTASTIC_EXCLUDE_GPS
    case gps_toggle_menu: GPSToggleMenu(); break;
#endif
    case compass_point_north_menu: compassNorthMenu(); break;
    case reset_node_db_menu: resetNodeDBMenu(); break;
    case buzzermodemenupicker: BuzzerModeMenu(); break;
    case mui_picker: switchToMUIMenu(); break;
    case tftcolormenupicker: TFTColorPickerMenu(display); break;
    case brightness_picker: BrightnessPickerMenu(); break;
    case reboot_menu: rebootMenu(); break;
    case shutdown_menu: shutdownMenu(); break;
    case add_favorite: addFavoriteMenu(); break;
    case remove_favorite: removeFavoriteMenu(); break;
    case trace_route_menu: traceRouteMenu(); break;
    case test_menu: testMenu(); break;
    case number_test: numberTest(); break;
    case wifi_toggle_menu: wifiToggleMenu(); break;
    case key_verification_init: keyVerificationInitMenu(); break;
    case key_verification_final_prompt: keyVerificationFinalPrompt(); break;
    case bluetooth_toggle_menu: BluetoothToggleMenu(); break;
    case notifications_menu: notificationsMenu(); break;
    case screen_options_menu: screenOptionsMenu(); break;
    case power_menu: powerMenu(); break;
    case FrameToggles: FrameToggles_menu(); break;
    case throttle_message:
        screen->showSimpleBanner("Too Many Attempts\nTry again in 60 seconds.", 5000);
        break;
    }
    menuQueue = menu_none;
}

/* ========================= SAVE UI CONFIG ========================= */

void menuHandler::saveUIConfig()
{
    nodeDB->saveProto("/prefs/uiconfig.proto", meshtastic_DeviceUIConfig_size, &meshtastic_DeviceUIConfig_msg, &uiconfig);
}

} // namespace graphics
#endif
