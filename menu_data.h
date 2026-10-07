#ifndef MENU_DATA_H
#define MENU_DATA_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound T-Embed CC1101 Plus — Menu Content
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 3)
//
// Text ported verbatim from HaleHound-CYD.ino's menu_items[]/*_submenu_items[]
// so feature parity is obvious at a glance; the corresponding CYD 16x16
// bitmaps are mapped below, and NUM items
// still counts the CYD build's trailing "Back to Main Menu" row even though
// this UI's BACK is the dedicated side button (§6.2) — kept for content
// parity; the renderer simply lets BACK exit a level early too.
//
// Structured as one MenuDef per top-level item so ui_render can walk a
// single array instead of a hand-written switch per category.
// ═══════════════════════════════════════════════════════════════════════════

struct MenuDef {
    const char *title;
    const char *const *items;
    int count;
};

static const char *const WIFI_SUBMENU[] = {
    "Packet Monitor", "Beacon Spammer", "WiFi Deauther", "Probe Sniffer",
    "WiFi Scanner", "Captive Portal", "Station Scanner", "Auth Flood",
    "Back to Main Menu",
};

// Named indices for the Phase 4 reference ports (packet_monitor.cpp,
// wifi_scan.cpp), the Phase 6 fidelity addition (deauther.cpp, a port of
// wifi_attacks.cpp's Deauther namespace's core scan->select->attack flow),
// and the fidelity-pass additions beacon_spammer.cpp (BeaconSpammer's TX
// core), auth_flood.cpp (AuthFlood's own scan->select->attack flow,
// structurally identical to deauther.cpp's), and station_scan.cpp
// (StationScan's passive client/AP-association discovery) — keeps the
// orchestrator's special-casing readable and resilient to WIFI_SUBMENU
// reordering.
#define WIFI_MENU_INDEX             0
#define WIFI_SUBMENU_PACKET_MONITOR 0
#define WIFI_SUBMENU_BEACON_SPAMMER 1
#define WIFI_SUBMENU_DEAUTHER       2
#define WIFI_SUBMENU_PROBE_SNIFFER  3
#define WIFI_SUBMENU_SCANNER        4
#define WIFI_SUBMENU_CAPTIVE_PORTAL 5
#define WIFI_SUBMENU_STATION_SCAN   6
#define WIFI_SUBMENU_AUTH_FLOOD     7

static const char *const BLUETOOTH_SUBMENU[] = {
    "BLE Jammer", "BLE Spoofer", "BLE Beacon", "BLE Predator",
    "WhisperPair", "Airoha RACE", "Lunatic Fringe", "Back to Main Menu",
};

// Named indices for the Phase 5 reference port (ble_scan.cpp), which
// implements exactly the "Phase 1: LISTEN" passive-scan behavior
// documented for BlePredator in bluetooth_attacks.h — not its Phase 2/3
// (target detail overlay, raw MAC/payload replay), which remain deferred.
#define BLUETOOTH_MENU_INDEX           1
#define BLUETOOTH_SUBMENU_BLE_JAMMER   0
#define BLUETOOTH_SUBMENU_BLE_SPOOFER  1
#define BLUETOOTH_SUBMENU_BLE_BEACON   2
#define BLUETOOTH_SUBMENU_BLE_PREDATOR 3
#define BLUETOOTH_SUBMENU_WHISPERPAIR  4
#define BLUETOOTH_SUBMENU_AIROHA_RACE  5
#define BLUETOOTH_SUBMENU_LUNATIC_FRINGE 6

static const char *const NRF_SUBMENU[] = {
    "Scanner", "Spectrum Analyzer", "NRF Sniffer", "MouseJack",
    "WLAN Jammer", "Proto Kill", "NRF Tuning", "Back to Main Menu",
};

static const char *const SUBGHZ_SUBMENU[] = {
    "Replay Attack", "Brute Force", "SubGHz Jammer", "Spectrum Analyzer",
    "Saved Profile", "Tesla Charge", ".Sub Read", "Back to Main Menu",
};

// Named indices for the Phase 5 reference port (subghz_scan.cpp) — a
// standalone RSSI-sweep implementation of subghz_attacks.cpp's SubAnalyzer
// (same frequency list/RSSI mapping), wired into the existing "Spectrum
// Analyzer" row — and the fidelity-pass additions subghz_jammer.cpp
// (SubJammer's TX core), subghz_brute.cpp (SubBrute's De Bruijn/Sequential
// code attack), subghz_replay.cpp (ReplayAttack's RMT-based capture/replay
// engine), subghz_tesla.cpp (TeslaCharge's fixed-payload OOK port opener),
// and subghz_subread.cpp (subread.cpp's .sub file browser/transmitter),
// wired into their existing-name rows.
#define SUBGHZ_MENU_INDEX           3
#define SUBGHZ_SUBMENU_REPLAY       0
#define SUBGHZ_SUBMENU_BRUTE        1
#define SUBGHZ_SUBMENU_JAMMER       2
#define SUBGHZ_SUBMENU_ANALYZER     3
#define SUBGHZ_SUBMENU_SAVED        4 // v3.9.x Phase 5: SD-backed saved-signal browser
#define SUBGHZ_SUBMENU_TESLA        5
#define SUBGHZ_SUBMENU_SUBREAD      6

static const char *const RFID_SUBMENU[] = {
    "Card Scanner", "Card Reader", "Card Clone", "Key Brute Force",
    "Card Emulate", "Back to Main Menu",
};

static const char *const JAMDETECT_SUBMENU[] = {
    "WiFi Guardian", "SubGHz Sentinel", "2.4GHz Watchdog", "Full Spectrum",
    "Back to Main Menu",
};

// Named indices for the Phase 5 reference port (subghz_sentinel.cpp, a
// reimplementation of jam_detect.cpp's SubSentinel namespace) and the
// fidelity-pass addition wifi_guardian.cpp (jam_detect.cpp's WiFiGuardian
// namespace), wired into their existing-name rows.
#define JAMDETECT_MENU_INDEX               5
#define JAMDETECT_SUBMENU_WIFI_GUARDIAN    0
#define JAMDETECT_SUBMENU_SUBGHZ_SENTINEL  1

static const char *const SIGINT_SUBMENU[] = {
    "EAPOL Capture", "Karma Attack", "Wardriving", "Saved Captures",
    "IoT Recon", "Loot", "Flock You", "Back to Main Menu",
};

// Named indices for the Phase 6 reference ports — wardrive_lite.cpp (a
// WiFi-only, no-GPS reimplementation of wardriving.cpp's scan+log loop)
// and eapol_sniff.cpp (a passive, listen-only slice of eapol_capture.cpp
// — no target selection, no deauth injection), wired into their
// existing-name rows.
#define SIGINT_MENU_INDEX              6
#define SIGINT_SUBMENU_EAPOL_CAPTURE   0
#define SIGINT_SUBMENU_KARMA           1
#define SIGINT_SUBMENU_WARDRIVING      2
#define SIGINT_SUBMENU_SAVED_CAPTURES  3
#define SIGINT_SUBMENU_IOT_RECON       4
#define SIGINT_SUBMENU_LOOT            5
#define SIGINT_SUBMENU_FLOCK_YOU       6

static const char *const TOOLS_SUBMENU[] = {
    "Serial Monitor", "Update Firmware", "GPS", "Radio Test",
    "Back to Main Menu",
};

static const char *const SETTINGS_SUBMENU[] = {
    "Brightness", "Screen Timeout", "Swap Colors", "Invert Display",
    "Color Mode", "Rotation", "Device Info", "Set PIN", "CC1101 Module",
    "Back to Main Menu",
};

// Named indices for the implemented Settings screens (the rest of the
// submenu's rows fall through to NAV_PLACEHOLDER).
#define SETTINGS_MENU_INDEX             8
#define SETTINGS_SUBMENU_BRIGHTNESS     0
#define SETTINGS_SUBMENU_TIMEOUT        1
#define SETTINGS_SUBMENU_SWAP           2
#define SETTINGS_SUBMENU_INVERT         3
#define SETTINGS_SUBMENU_COLOR_MODE     4
#define SETTINGS_SUBMENU_ROTATION       5
#define SETTINGS_SUBMENU_DEVICE_INFO    6
#define SETTINGS_SUBMENU_PIN            7
#define SETTINGS_SUBMENU_CC1101         8

static const char *const ABOUT_SUBMENU[] = {
    "Back to Main Menu",
};

#define ABOUT_MENU_INDEX                9

#define NUM_MENU_ITEMS 10

#define NRF_MENU_INDEX                  2
#define RFID_MENU_INDEX                 4
#define RFID_SUBMENU_SCANNER            0
#define RFID_SUBMENU_READER             1
#define TOOLS_MENU_INDEX                7

// Named indices for the NRF24 ports (nrf_scanner.cpp first; the remaining
// rows stay placeholders until their modules are ported).
#define NRF_SUBMENU_SCANNER             0
#define NRF_SUBMENU_ANALYZER            1
#define NRF_SUBMENU_SNIFFER             2
#define NRF_SUBMENU_MOUSEJACK           3
#define NRF_SUBMENU_WLAN_JAMMER         4
#define NRF_SUBMENU_PROTO_KILL          5
#define NRF_SUBMENU_TUNING              6 // v3.9.x Phase 7

static const MenuDef MAIN_MENU[NUM_MENU_ITEMS] = {
    {"WiFi",       WIFI_SUBMENU,      sizeof(WIFI_SUBMENU) / sizeof(char*)},
    {"Bluetooth",  BLUETOOTH_SUBMENU, sizeof(BLUETOOTH_SUBMENU) / sizeof(char*)},
    {"2.4GHz",     NRF_SUBMENU,       sizeof(NRF_SUBMENU) / sizeof(char*)},
    {"SubGHz",     SUBGHZ_SUBMENU,    sizeof(SUBGHZ_SUBMENU) / sizeof(char*)},
    {"RFID",       RFID_SUBMENU,      sizeof(RFID_SUBMENU) / sizeof(char*)},
    {"Jam Detect", JAMDETECT_SUBMENU, sizeof(JAMDETECT_SUBMENU) / sizeof(char*)},
    {"SIGINT",     SIGINT_SUBMENU,    sizeof(SIGINT_SUBMENU) / sizeof(char*)},
    {"Tools",      TOOLS_SUBMENU,     sizeof(TOOLS_SUBMENU) / sizeof(char*)},
    {"Setting",    SETTINGS_SUBMENU,  sizeof(SETTINGS_SUBMENU) / sizeof(char*)},
    {"About",      ABOUT_SUBMENU,     sizeof(ABOUT_SUBMENU) / sizeof(char*)},
};

// ═══════════════════════════════════════════════════════════════════════════
// MENU ICONS (CYD parity)
//
// icon.h is included by exactly one translation unit (ui_render.cpp) so the
// PROGMEM bitmap definitions exist once; here we only declare the ones this
// menu references. Only referenced bitmaps are linked.
// ═══════════════════════════════════════════════════════════════════════════

extern const unsigned char bitmap_icon_skull_wifi[] PROGMEM;
extern const unsigned char bitmap_icon_skull_bluetooth[] PROGMEM;
extern const unsigned char bitmap_icon_skull_jammer[] PROGMEM;
extern const unsigned char bitmap_icon_skull_subghz[] PROGMEM;
extern const unsigned char bitmap_icon_skull_rfid[] PROGMEM;
extern const unsigned char bitmap_icon_skull_tools[] PROGMEM;
extern const unsigned char bitmap_icon_skull_ir[] PROGMEM;
extern const unsigned char bitmap_icon_skull_setting[] PROGMEM;
extern const unsigned char bitmap_icon_skull_about[] PROGMEM;
extern const unsigned char bitmap_icon_wifi[] PROGMEM;
extern const unsigned char bitmap_icon_antenna[] PROGMEM;
extern const unsigned char bitmap_icon_wifi_jammer[] PROGMEM;
extern const unsigned char bitmap_icon_jammer[] PROGMEM;
extern const unsigned char bitmap_icon_bash[] PROGMEM;
extern const unsigned char bitmap_icon_graph[] PROGMEM;
extern const unsigned char bitmap_icon_nuke[] PROGMEM;
extern const unsigned char bitmap_icon_go_back[] PROGMEM;
extern const unsigned char bitmap_icon_ble_jammer[] PROGMEM;
extern const unsigned char bitmap_icon_spoofer[] PROGMEM;
extern const unsigned char bitmap_icon_signal[] PROGMEM;
extern const unsigned char bitmap_icon_analyzer[] PROGMEM;
extern const unsigned char bitmap_icon_eye[] PROGMEM;
extern const unsigned char bitmap_icon_eye2[] PROGMEM;
extern const unsigned char bitmap_icon_key[] PROGMEM;
extern const unsigned char bitmap_icon_scanner[] PROGMEM;
extern const unsigned char bitmap_icon_apple[] PROGMEM;
extern const unsigned char bitmap_icon_no_signal[] PROGMEM;
extern const unsigned char bitmap_icon_list[] PROGMEM;
extern const unsigned char bitmap_icon_flash[] PROGMEM;
extern const unsigned char bitmap_icon_floppy[] PROGMEM;
extern const unsigned char bitmap_icon_floppy2[] PROGMEM;
extern const unsigned char bitmap_icon_rfid[] PROGMEM;
extern const unsigned char bitmap_icon_follow[] PROGMEM;
extern const unsigned char bitmap_icon_stat[] PROGMEM;
extern const unsigned char bitmap_icon_led[] PROGMEM;
extern const unsigned char bitmap_icon_recycle[] PROGMEM;
extern const unsigned char bitmap_icon_dialog[] PROGMEM;

static const unsigned char *const MAIN_MENU_ICONS[NUM_MENU_ITEMS] = {
    bitmap_icon_skull_wifi,
    bitmap_icon_skull_bluetooth,
    bitmap_icon_skull_jammer,
    bitmap_icon_skull_subghz,
    bitmap_icon_skull_rfid,
    bitmap_icon_skull_tools,
    bitmap_icon_skull_ir,
    bitmap_icon_skull_tools,
    bitmap_icon_skull_setting,
    bitmap_icon_skull_about
};

static const unsigned char *const WIFI_SUBMENU_ICONS[] = {
    bitmap_icon_wifi, bitmap_icon_antenna, bitmap_icon_wifi_jammer,
    bitmap_icon_skull_wifi, bitmap_icon_jammer, bitmap_icon_bash,
    bitmap_icon_graph, bitmap_icon_nuke, bitmap_icon_go_back
};

static const unsigned char *const BLUETOOTH_SUBMENU_ICONS[] = {
    bitmap_icon_ble_jammer, bitmap_icon_spoofer, bitmap_icon_signal,
    bitmap_icon_analyzer, bitmap_icon_eye, bitmap_icon_key,
    bitmap_icon_scanner, bitmap_icon_go_back
};

static const unsigned char *const NRF_SUBMENU_ICONS[] = {
    bitmap_icon_scanner, bitmap_icon_analyzer, bitmap_icon_eye,
    bitmap_icon_nuke, bitmap_icon_wifi_jammer, bitmap_icon_skull_jammer,
    bitmap_icon_led, // NRF Tuning (v3.9.x Phase 7) — matches the "adjustable
                     // setting" icon Settings' Brightness/Swap/Invert/ColorMode rows already use
    bitmap_icon_go_back
};

static const unsigned char *const SUBGHZ_SUBMENU_ICONS[] = {
    bitmap_icon_antenna, bitmap_icon_skull_subghz, bitmap_icon_no_signal,
    bitmap_icon_analyzer, bitmap_icon_list, bitmap_icon_flash,
    bitmap_icon_signal, bitmap_icon_go_back
};

static const unsigned char *const RFID_SUBMENU_ICONS[] = {
    bitmap_icon_scanner, bitmap_icon_list, bitmap_icon_floppy,
    bitmap_icon_key, bitmap_icon_rfid, bitmap_icon_go_back
};

static const unsigned char *const JAMDETECT_SUBMENU_ICONS[] = {
    bitmap_icon_skull_wifi, bitmap_icon_skull_subghz, bitmap_icon_skull_jammer,
    bitmap_icon_analyzer, bitmap_icon_go_back
};

static const unsigned char *const SIGINT_SUBMENU_ICONS[] = {
    bitmap_icon_key, bitmap_icon_spoofer, bitmap_icon_follow,
    bitmap_icon_floppy2, bitmap_icon_scanner, bitmap_icon_floppy,
    bitmap_icon_scanner, bitmap_icon_go_back
};

static const unsigned char *const TOOLS_SUBMENU_ICONS[] = {
    bitmap_icon_bash, bitmap_icon_follow, bitmap_icon_stat,
    bitmap_icon_antenna, bitmap_icon_signal, bitmap_icon_go_back
};

static const unsigned char *const SETTINGS_SUBMENU_ICONS[] = {
    bitmap_icon_led, bitmap_icon_eye2, bitmap_icon_led,
    bitmap_icon_led, bitmap_icon_led, bitmap_icon_follow,
    bitmap_icon_stat, bitmap_icon_eye2, bitmap_icon_antenna,
    bitmap_icon_go_back
};

static const unsigned char *const ABOUT_SUBMENU_ICONS[] = {
    bitmap_icon_go_back
};

// ═══════════════════════════════════════════════════════════════════════════
// CARDPUTER ADV MENU — WiFi/BLE-only this milestone (see the port plan §9).
// Reuses WIFI_SUBMENU/WIFI_SUBMENU_ICONS and ABOUT_SUBMENU/ABOUT_SUBMENU_ICONS
// verbatim (full parity, nothing trimmed there); Bluetooth/Jam Detect/SIGINT/
// Settings get trimmed parallel tables below so this never touches the
// T-Embed arrays above. Menu entries for anything not ported this pass
// (SubGHz/NRF/RFID/Tools, and every Bluetooth/Jam Detect/SIGINT row without
// a real feature behind it) are simply omitted rather than left as
// placeholders — the port plan calls for that explicitly (§9).
// ═══════════════════════════════════════════════════════════════════════════
#if defined(BOARD_CARDPUTER_ADV) || defined(BOARD_M5STICK_S3)
// M5Stick S3 reuses the Cardputer's 240x135 menu layout, with a fuller
// SIGINT submenu selected below for its LittleFS-backed WiFi/BLE tools.

static const char *const CARDPUTER_BT_SUBMENU[] = {
    "BLE Spoofer", "BLE Beacon", "BLE Predator", "Lunatic Fringe",
    "Back to Main Menu",
};
#define CARDPUTER_BT_SUBMENU_BLE_SPOOFER     0
#define CARDPUTER_BT_SUBMENU_BLE_BEACON      1
#define CARDPUTER_BT_SUBMENU_BLE_PREDATOR    2
#define CARDPUTER_BT_SUBMENU_LUNATIC_FRINGE  3  // opens the picker below

// "Lunatic Fringe" picker — selecting the row above opens this static list
// instead of launching a scan directly, same two-step shape as the BLE
// Spoofer payload picker (NAV_BLE_SPOOFER_MENU in board_bringup_*.cpp). Item
// set/order matches the reference HaleHound build's own Lunatic Fringe
// submenu. Only Tracker Scan and AirTag Detect have a working backend
// (lunatic_fringe.cpp / airtag_tracker.cpp) — Phantom Flood/AirTag Replay/
// AirTag Scream are not ported yet and fall through to NAV_PLACEHOLDER,
// same convention as every other not-yet-ported row in this project.
static const char *const LUNATIC_FRINGE_SUBMENU[] = {
    "Tracker Scan",
#ifdef BOARD_M5STICK_S3
    "AirTag Detect", "Phantom Flood", "AirTag Replay", "AirTag Scream",
#endif
};
#define LUNATIC_FRINGE_SUBMENU_TRACKER_SCAN   0  // all tracker types (lunatic_fringe.cpp)
#ifdef BOARD_M5STICK_S3
#define LUNATIC_FRINGE_SUBMENU_AIRTAG_DETECT  1  // AirTag/FindMy-only (airtag_tracker.cpp), M5Stick S3 only
#define LUNATIC_FRINGE_SUBMENU_PHANTOM_FLOOD  2  // not ported yet — placeholder
#define LUNATIC_FRINGE_SUBMENU_AIRTAG_REPLAY  3  // not ported yet — placeholder
#define LUNATIC_FRINGE_SUBMENU_AIRTAG_SCREAM  4  // not ported yet — placeholder
#endif
#define LUNATIC_FRINGE_SUBMENU_COUNT (sizeof(LUNATIC_FRINGE_SUBMENU) / sizeof(char*))

// Icons: Tracker Scan/AirTag Detect reuse the exact bitmaps the old flat
// Bluetooth-submenu rows used for these same two features; Phantom Flood
// borrows Auth Flood's "nuke" (WIFI_SUBMENU_ICONS) since both are flood-type
// attacks; AirTag Replay gets the loop/recycle glyph (replays a captured
// advertisement); AirTag Scream gets the speech-bubble glyph (makes the
// tracker emit a sound/alert).
static const unsigned char *const LUNATIC_FRINGE_SUBMENU_ICONS[] = {
    bitmap_icon_scanner,
#ifdef BOARD_M5STICK_S3
    bitmap_icon_apple, bitmap_icon_nuke, bitmap_icon_recycle, bitmap_icon_dialog,
#endif
};

static const char *const CARDPUTER_JAMDETECT_SUBMENU[] = {
    "WiFi Guardian", "Back to Main Menu",
};
#define CARDPUTER_JAMDETECT_SUBMENU_WIFI_GUARDIAN 0

#ifdef BOARD_M5STICK_S3
// The StickS3 has flash-backed captures and the on-chip radios needed for
// every SIGINT entry. Keep Cardputer's narrower menu unchanged.
#define CARDPUTER_SIGINT_SUBMENU SIGINT_SUBMENU
#define CARDPUTER_SIGINT_SUBMENU_EAPOL_CAPTURE SIGINT_SUBMENU_EAPOL_CAPTURE
#define CARDPUTER_SIGINT_SUBMENU_KARMA SIGINT_SUBMENU_KARMA
#define CARDPUTER_SIGINT_SUBMENU_WARDRIVING SIGINT_SUBMENU_WARDRIVING
#define CARDPUTER_SIGINT_SUBMENU_SAVED_CAPTURES SIGINT_SUBMENU_SAVED_CAPTURES
#define CARDPUTER_SIGINT_SUBMENU_IOT_RECON SIGINT_SUBMENU_IOT_RECON
#define CARDPUTER_SIGINT_SUBMENU_LOOT SIGINT_SUBMENU_LOOT
#define CARDPUTER_SIGINT_SUBMENU_FLOCK_YOU SIGINT_SUBMENU_FLOCK_YOU
#else
static const char *const CARDPUTER_SIGINT_SUBMENU[] = {
    "EAPOL Capture", "Back to Main Menu",
};
#define CARDPUTER_SIGINT_SUBMENU_EAPOL_CAPTURE 0
#endif

// Indices 0-7 intentionally match SETTINGS_SUBMENU_* above — only the
// CC1101 Module row (out of scope on this board, no on-board CC1101) is
// dropped, so the existing SETTINGS_SUBMENU_BRIGHTNESS..SETTINGS_SUBMENU_PIN
// #defines are reused as-is rather than duplicated under a CARDPUTER_ name.
static const char *const CARDPUTER_SETTINGS_SUBMENU[] = {
    "Brightness", "Screen Timeout", "Swap Colors", "Invert Display",
    "Color Mode", "Rotation", "Device Info", "Set PIN",
    "Back to Main Menu",
};

#define CARDPUTER_WIFI_MENU_INDEX       0
#define CARDPUTER_BT_MENU_INDEX         1
#define CARDPUTER_JAMDETECT_MENU_INDEX  2
#define CARDPUTER_SIGINT_MENU_INDEX     3
#define CARDPUTER_SETTINGS_MENU_INDEX   4
#define CARDPUTER_ABOUT_MENU_INDEX      5
#define CARDPUTER_NUM_MENU_ITEMS        6

static const MenuDef CARDPUTER_MAIN_MENU[CARDPUTER_NUM_MENU_ITEMS] = {
    {"WiFi",       WIFI_SUBMENU,                sizeof(WIFI_SUBMENU) / sizeof(char*)},
    {"Bluetooth",  CARDPUTER_BT_SUBMENU,        sizeof(CARDPUTER_BT_SUBMENU) / sizeof(char*)},
    {"Jam Detect", CARDPUTER_JAMDETECT_SUBMENU, sizeof(CARDPUTER_JAMDETECT_SUBMENU) / sizeof(char*)},
    {"SIGINT",     CARDPUTER_SIGINT_SUBMENU,    sizeof(CARDPUTER_SIGINT_SUBMENU) / sizeof(char*)},
    {"Setting",    CARDPUTER_SETTINGS_SUBMENU,  sizeof(CARDPUTER_SETTINGS_SUBMENU) / sizeof(char*)},
    {"About",      ABOUT_SUBMENU,               sizeof(ABOUT_SUBMENU) / sizeof(char*)},
};

static const unsigned char *const CARDPUTER_MAIN_MENU_ICONS[CARDPUTER_NUM_MENU_ITEMS] = {
    bitmap_icon_skull_wifi, bitmap_icon_skull_bluetooth, bitmap_icon_skull_tools,
    bitmap_icon_skull_ir, bitmap_icon_skull_setting, bitmap_icon_skull_about,
};

static const unsigned char *const CARDPUTER_BT_SUBMENU_ICONS[] = {
    bitmap_icon_spoofer, bitmap_icon_signal, bitmap_icon_analyzer,
    bitmap_icon_scanner, bitmap_icon_go_back,
};

static const unsigned char *const CARDPUTER_JAMDETECT_SUBMENU_ICONS[] = {
    bitmap_icon_skull_wifi, bitmap_icon_go_back,
};

#ifdef BOARD_M5STICK_S3
#define CARDPUTER_SIGINT_SUBMENU_ICONS SIGINT_SUBMENU_ICONS
#else
static const unsigned char *const CARDPUTER_SIGINT_SUBMENU_ICONS[] = {
    bitmap_icon_key, bitmap_icon_go_back,
};
#endif

static const unsigned char *const CARDPUTER_SETTINGS_SUBMENU_ICONS[] = {
    bitmap_icon_led, bitmap_icon_eye2, bitmap_icon_led, bitmap_icon_led,
    bitmap_icon_led, bitmap_icon_follow, bitmap_icon_stat, bitmap_icon_eye2,
    bitmap_icon_go_back,
};

#endif // BOARD_CARDPUTER_ADV || BOARD_M5STICK_S3

#endif // MENU_DATA_H
