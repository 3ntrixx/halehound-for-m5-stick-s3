#ifndef AIRTAG_TRACKER_H
#define AIRTAG_TRACKER_H

// AirTag-only passive BLE scan (anti-stalking aid). Mirrors lunatic_fringe.h.
void airtagTrackerStart();
void airtagTrackerStop(); // blocks until the worker exits and the radio is released
const char *const *airtagTrackerGetResultLines();
int airtagTrackerGetResultCount();

#endif // AIRTAG_TRACKER_H
