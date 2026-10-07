#!/usr/bin/env python3
"""
HaleHound OUI registry builder — generates oui.bin for oui_registry.cpp.

Input: IEEE's public MA-L (24-bit OUI) assignment registry, as published at
https://standards-oui.ieee.org/oui/oui.csv — columns:
    Registry,Assignment,Organization Name,Organization Address
"Assignment" is the 6 hex-digit OUI (no separators), e.g. "3C0754".

Output format (must match oui_registry.cpp's reader exactly):
    Header (12 bytes):
        magic       4 bytes  "HHOU"
        version     1 byte   1
        recordSize  1 byte   24
        reserved    2 bytes  0x00 0x00
        count       4 bytes  little-endian uint32, number of records
    Records (count * 24 bytes), sorted ascending by the 3-byte OUI so the
    reader can binary-search them, each:
        oui         3 bytes  first 3 octets of a MAC, as assigned by IEEE
        name        21 bytes NUL-terminated vendor name, NUL-padded

21 bytes leaves 20 visible characters plus the terminator — organization
names longer than that are truncated. Fine for a glanceable UI column; not
meant to be the full legal entity name.

Usage:
    python tools/build_oui_registry.py oui.csv data/oui.bin [--full]

Without --full, only OUI blocks belonging to vendors on the curated
KEYWORDS allow-list below are kept (case-insensitive substring match against
IEEE's "Organization Name" field) — phones/laptops, routers/APs, common
WiFi/BLE chipset & module makers, and IoT/smart-home brands, i.e. the
vendors station_scan.cpp / probe_sniffer.cpp actually expect to see on air.
This trades completeness (an obscure or white-label device may still show
"???", same as before this feature existed) for a much smaller image, since
the StickS3's LittleFS partition is shared with wardrive logs and captures
rather than being dedicated read-only flash. Pass --full to instead emit
every MA-L assignment (~900KB+ as of the registry's current size) if that
trade-off ever changes.
"""

import csv
import re
import struct
import sys

MAGIC = b"HHOU"
VERSION = 1
RECORD_SIZE = 24
NAME_FIELD = RECORD_SIZE - 3  # 21 bytes
NAME_MAX_CHARS = NAME_FIELD - 1  # 20 visible chars + NUL

# Curated, case-insensitive substring allow-list. Deliberately broad per
# brand (catches "Apple, Inc." / "APPLE COMPUTER" / etc.) rather than exact
# -- false positives here just mean a harmless extra OUI block; false
# negatives mean a device still falls back to "???" same as pre-existing
# behavior. Grouped by why a MAC with that OUI shows up in a WiFi/BLE scan:
KEYWORDS = [
    # ── Phones / tablets ────────────────────────────────────────────────
    "apple", "samsung", "huawei", "xiaomi", "oppo", "oneplus", " vivo",
    "honor device", "realme", "motorola", "lg electronics", "sony mobile",
    "sony corporation", "nokia", "hmd global", "zte corporation", "htc",
    "blackberry", "nothing technology", "meizu", "google",

    # ── Laptops / PCs / peripherals ─────────────────────────────────────
    "dell inc", "hewlett packard", "hp inc", "lenovo", "asustek",
    "acer inc", "microsoft", "toshiba", "micro-star international",
    "fujitsu", "razer", "gigabyte", "panasonic", "framework computer",
    "logitech", "quanta computer", "wistron", "compal", "pegatron",
    "hon hai", "foxconn",

    # ── Routers / APs / networking gear ─────────────────────────────────
    "cisco", "tp-link", "netgear", "d-link", "linksys", "ubiquiti",
    "belkin", "arris", "technicolor", "actiontec", "zyxel", "mikrotik",
    "aruba", "ruckus", "juniper", "fortinet", "avm gmbh", "sagemcom",
    "sercomm", "comtrend", "hitron", "netcomm", "tenda", "mercusys",
    "edimax",

    # ── WiFi/BLE chipset & module vendors (the OUI on an integrated
    #    radio often belongs to the chip/module maker, not the end brand)
    "intel corp", "realtek", "broadcom", "qualcomm", "murata",
    "azurewave", "u-blox", "espressif", "nordic semiconductor",
    "texas instruments", "silicon laboratories", "microchip technology",
    "cypress semiconductor", "infineon", "mediatek", "marvell",
    "atheros", "ralink",

    # ── IoT / smart-home brands ──────────────────────────────────────────
    "amazon technologies", "amazon.com", "nest labs", "ring llc",
    "sonos", "roku", "signify", "wemo", "tuya smart", "ecobee",
    "arlo technologies", "wyze labs", "smartthings", "anker innovations",
    "lifx", "shelly", "irobot", "fitbit", "garmin international",
    "withings", "oura health", "ecovacs",

    # ── SBC / dev boards (common attacker/IoT-prototype gear on air) ────
    "raspberry pi", "particle industries", "arduino", "seeed technology",
    "nxp semiconductors", "stmicroelectronics",

    # ── Gaming / media ───────────────────────────────────────────────────
    "nintendo", "valve corporation",
]


def normalize_oui(assignment: str) -> bytes:
    assignment = assignment.strip().upper()
    if not re.fullmatch(r"[0-9A-F]{6}", assignment):
        return None
    return bytes.fromhex(assignment)


def truncate_name(name: str) -> bytes:
    # Collapse whitespace/control chars IEEE's CSV sometimes carries, keep
    # the record ASCII-safe (the UI renders this in a fixed-width font).
    name = re.sub(r"\s+", " ", name).strip()
    name = name.encode("ascii", "replace").decode("ascii")
    name = name[:NAME_MAX_CHARS]
    field = name.encode("ascii") + b"\x00"
    return field.ljust(NAME_FIELD, b"\x00")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    full = "--full" in sys.argv
    if len(args) != 2:
        print(__doc__)
        sys.exit(1)
    csv_path, out_path = args

    records = {}
    total_rows = 0
    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            total_rows += 1
            org = row.get("Organization Name", "")
            if not full:
                org_lower = org.lower()
                if not any(kw in org_lower for kw in KEYWORDS):
                    continue
            oui = normalize_oui(row.get("Assignment", ""))
            if oui is None:
                continue
            # Duplicate OUI across rows (rare) — keep the first seen.
            records.setdefault(oui, org)

    sorted_ouis = sorted(records.keys())
    count = len(sorted_ouis)
    if count == 0:
        print("No matching records — check the keyword list / input file.")
        sys.exit(1)

    with open(out_path, "wb") as out:
        header = MAGIC + bytes([VERSION, RECORD_SIZE, 0, 0]) + struct.pack("<I", count)
        assert len(header) == 12
        out.write(header)
        for oui in sorted_ouis:
            out.write(oui + truncate_name(records[oui]))

    size = 12 + count * RECORD_SIZE
    print(f"Scanned {total_rows} IEEE MA-L rows -> kept {count} "
          f"({'full list' if full else 'curated subset'}).")
    print(f"Wrote {out_path}: {size} bytes ({size / 1024:.1f} KB).")


if __name__ == "__main__":
    main()
