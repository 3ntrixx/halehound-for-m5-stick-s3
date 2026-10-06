#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Captive Portal — reference port
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
//
// A full-fidelity port of wifi_attacks.cpp's CaptivePortal namespace: an
// open evil-twin AP (WiFi.softAP, no password) serving one of the
// original's 11 fake login page templates (portal_pages.h, reused
// unmodified — it's pure PROGMEM HTML/CSS with no CYD-specific code) via
// DNSServer (wildcard DNS spoof) + WebServer, with the same captive-
// portal-detection endpoints (/generate_204, /hotspot-detect.html,
// /ncsi.txt, /connecttest.txt) and catch-all redirect. Multi-stage
// templates (Google/Microsoft: email -> password -> MFA) work exactly as
// the original's {{EMAIL}}/{{SSID}} placeholder substitution does. While
// active, a background task continuously deauths the real AP's clients
// (same broadcast deauth frame as Deauther, sent through the evil twin's
// own already-up AP interface — no separate hidden-AP teardown/rebuild
// needed here, unlike Deauther/AuthFlood, since this AP must stay up and
// visible the whole time) to push them onto the evil twin.
//
// Encoder-native SSID naming: this board has no touchscreen keyboard, so
// naming the fake AP uses a character-wheel instead of the original's
// on-screen QWERTY grid — rotate cycles the character at the cursor
// (A-Z, a-z, 0-9, -, _, space, then a backspace sentinel), press commits
// it and advances the cursor, long-press finishes editing, BACK cancels.
// This is the same class of adaptation as every touch control ported
// elsewhere in this project, not a scope cut — the resulting capability
// (name the evil twin anything) is unchanged.
//
// Not ported: the in-app Saved Credentials viewer/delete UI and EEPROM
// storage. Captured credentials are appended to /creds.txt via
// storage_manager.cpp (the same durable, human-readable record the
// original also writes to SD — "SSID | email | password | mfa"),
// viewable by pulling the card. This matches the "Saved Profile" viewer
// already deferred for SubGHz for the same reason: an in-app data browser
// is a separate concern from the capture mechanism itself.
// ═══════════════════════════════════════════════════════════════════════════

void captivePortalReadyEnter();  // menu entry — pure state, no radio touched yet
void captivePortalStart();       // begins the evil twin (scans for the real AP, brings up AP+DNS+web+deauth)
void captivePortalStop();

// Pre-seed the SSID buffer before captivePortalReadyEnter() — used by the
// Karma Attack handoff (karma_attack_tembed.h), equivalent to the CYD's
// CaptivePortal::setSSID() entry point.  Safe to call outside the feature
// (pure state write, no radio touched).
void captivePortalSetSSID(const char *ssid);

// READY-phase controls (pure state, safe to call directly).
void captivePortalAdjustTemplate(int delta); // encoder rotate: cycle template
void captivePortalEditSSIDBegin();           // encoder long-press: enter SSID edit
const char *captivePortalGetSSID();
const char *captivePortalGetTemplateName();

// SSID edit-phase controls.
void captivePortalEditRotate(int delta);  // cycle the character at the cursor
void captivePortalEditCommitChar();       // encoder press: commit char / backspace
void captivePortalEditFinish();           // encoder long-press: apply and exit edit mode
void captivePortalEditCancel();           // BACK: discard and exit edit mode
const char *captivePortalGetEditDisplay();

// ACTIVE-phase live log (client connects / captured credentials).
const char *const *captivePortalGetLogLines();
int captivePortalGetLogCount();

#endif // CAPTIVE_PORTAL_H
