#ifndef AIRTAG_TRACKER_H
#define AIRTAG_TRACKER_H

#include "ui_render.h" // TrackerDisplayRow / UI_TRACKER_MAX_ROWS

// Apple Find My network passive BLE scan (anti-stalking aid): genuine AirTags
// plus other Find My network accessories, each row showing a decoded battery
// level (AirTag's status byte, see airtag_tracker.cpp's classifyAndTrack()).
// Mirrors lunatic_fringe.h, filtered to Apple-only. UI: ui_render.h's
// uiRenderShowTrackerScanning()/ShowTrackerList(), screen-for-screen port of
// the reference build's own AIRTAG screen (persistent TRACKERS/SCANS header,
// unlike Lunatic Fringe's list).
void airtagTrackerStart();
void airtagTrackerStop(); // blocks until the worker exits and the radio is released
int airtagTrackerGetDeviceCount();  // raw count, 0 when nothing found yet
int airtagTrackerGetScanCount();    // completed 5s scan windows this session
int airtagTrackerGetDisplayRows(TrackerDisplayRow *out, int max);

#endif // AIRTAG_TRACKER_H
