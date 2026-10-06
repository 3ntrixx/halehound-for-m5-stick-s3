// ═══════════════════════════════════════════════════════════════════════════
// HaleHound M5Stack StickS3 — TFT_eSPI Configuration
// ST7789P3 135x240 native panel, run landscape (setRotation(1) -> 240x135),
// own SPI bus — not shared with any radio or storage pin on this board.
// Created: 2026-10-04 (StickS3 port, Stage 1 — board bring-up)
//
// Pin values sourced from board_m5stick_s3.h — do not hardcode here; this
// file must always mirror that header's CYD_TFT_* macros.
//
// NOT hardware-verified — see the outline's bring-up milestone (correct
// colors/orientation on real hardware) before trusting the offsets/polarity
// implied by ST7789_DRIVER + TFT_INVERSION_ON below. If the first boot shows
// a shifted image, a garbage column, or inverted colors, this is the file
// (and, failing that, this env's own vendored copy of
// TFT_Drivers/ST7789_Rotation.h under .pio/libdeps/m5stick-s3/TFT_eSPI/) to
// correct — see User_Setup_Cardputer.h's own note for why that vendored
// copy is the right place and why it's isolated per-env.
// ═══════════════════════════════════════════════════════════════════════════

#define USER_SETUP_INFO "HaleHound-StickS3"

#define ST7789_DRIVER
#define TFT_WIDTH   135
#define TFT_HEIGHT  240
#define CGRAM_OFFSET  // enable TFT_eSPI's 135x240 panel offsets
#define CYD_BOARD_NAME "M5Stack StickS3"

// Same S3 direct-register bug the T-Embed/Cardputer ADV ports both hit (see
// User_Setup_TEmbed.h for the full explanation): TFT_eSPI 2.5.43's ESP32-S3
// processor file defaults to FSPI (SPI_PORT=0), whose REG_SPI_BASE() math is
// wrong for the direct-register write path on this IDF and panics tft.init()
// with StoreProhibited unless an explicit HSPI/VSPI port flag is set. Using
// the same flag the other two S3 ports use; if hardware shows this board's
// panel is actually wired to a different SPI3_HOST-equivalent unit, this is
// the line to revisit.
#define USE_HSPI_PORT

#define TFT_MISO    -1   // 3-wire bus, no MISO
#define TFT_MOSI    39
#define TFT_SCLK    40
#define TFT_CS      41
#define TFT_DC      45
#define TFT_RST     21
#define TFT_BL      38
#define TFT_BACKLIGHT_ON HIGH

#define TFT_INVERSION_ON

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT

#define SPI_FREQUENCY        40000000
#define SPI_READ_FREQUENCY   16000000
