/**
 * @file screen_renderer_esp32.cpp
 * @brief Implementation of the device screen renderer (ADR-0025).
 */
#include "surface/screen_renderer_esp32.h"

#include <cstdio>
#include <cstring>

#include "config/firmware_version.h"  // kFirmwareVersion — the single version source the splash uses too.
#include "surface/wordmark.h"          // drawWordmark — the shared product wordmark (slice-0097).

namespace sapper {
namespace {

// RGB565 palette. The status colours match the LED's intent (green hunting, blue working, amber
// degraded, red fault) so the two surfaces read consistently; the banner is high-contrast white so a
// crack is unmistakable at a glance.
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kBlue = 0x001F;
constexpr uint16_t kAmber = 0xFD20;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kCyan = 0x07FF;  // the live-hunt SCAN line (ADR-0033).
constexpr uint16_t kDim = 0x8410;   // grey for a not-yet-happened field (e.g. no sync yet, an absent M-frame).

// The default LovyanGFX glyph is ~6x8 px at size 1, so a field at integer `size` is 6*size wide, 8*size
// tall. Used to lay out rows and to position the version string after the product name without a
// textWidth() the seam does not expose (the on-air photo is the eyeball proof, ADR-0025).
constexpr int32_t kGlyphW = 6;
constexpr int32_t kGlyphH = 8;
constexpr int32_t kDotPadY = 2;  // the heartbeat square sits a touch below the status baseline.

const char kTitleName[] = "WiFi Sapper";

/**
 * @brief One panel-size class's vertical layout, font sizes, and margins (slice-0088).
 *
 * Horizontal extents that depend on the exact panel width (bar width, field-clear rects, the banner
 * strip) are taken from display_.width() at draw time, so one tier serves a range of widths and the
 * T-embed's 320 px rows span its panel instead of stopping at the old hardcoded 240. These fields are
 * candidate values photo-tuned on the physical panels (ADR-0002 §5); the tier *selection* by panel
 * size is the invariant, not the exact numbers.
 */
struct Layout {
    int32_t margin;
    int32_t titleY;
    uint8_t titleSize;
    uint8_t versionSize;  ///< The "vX.Y.Z" string, drawn smaller after the product name.
    int32_t statusY;
    uint8_t statusSize;
    int32_t dotSize;      ///< The heartbeat square beside the status word.
    int32_t countersY;
    uint8_t lineSize;     ///< Body text (counters, sync, hunt label/indicators).
    int32_t syncY;
    int32_t huntY;        ///< SCAN line / idle / the capturing target label.
    int32_t huntIndY;     ///< The B/M1-M4 indicator row. Equal to huntY => share the label line (compact).
    int32_t huntBarY;
    int32_t huntBarH;
    int32_t indStep;      ///< Per-indicator advance when the row is on its own line (roomy).
    int32_t bannerY;      ///< The CAPTURED/CRACKED strip, drawn last over the lower HUD.
    uint8_t bannerTitleSize;
    int32_t bannerTitleY;
    int32_t bannerEssidY;
    int32_t bannerBssidY;
};

// Compact tier: 240x135 (Cardputer, M5StickC Plus2). The pre-slice-0088 layout, preserved so those
// already-tuned panels do not regress; the only visible gain is the version string in the title. The
// indicator row shares the label line (huntIndY == huntY) because 240x135 has no room for its own row
// before the banner.
constexpr Layout kCompact{
    /*margin*/ 4,
    /*titleY*/ 2, /*titleSize*/ 2, /*versionSize*/ 1,
    /*statusY*/ 26, /*statusSize*/ 2, /*dotSize*/ 10,
    /*countersY*/ 52, /*lineSize*/ 1,
    /*syncY*/ 68,
    /*huntY*/ 76, /*huntIndY*/ 76, /*huntBarY*/ 84, /*huntBarH*/ 2, /*indStep*/ 2 * kGlyphW,
    /*bannerY*/ 86, /*bannerTitleSize*/ 2, /*bannerTitleY*/ 90, /*bannerEssidY*/ 110, /*bannerBssidY*/ 122,
};

// Roomy tier: a taller/wider sprite panel (the LilyGo T-embed, 320x170). Larger title/status/body
// fonts and the extra vertical room the compact tier lacks, so the HUD fills the panel instead of the
// top-left 240x135 corner. The indicator row gets its own line (huntIndY != huntY).
constexpr Layout kRoomy{
    /*margin*/ 6,
    /*titleY*/ 4, /*titleSize*/ 3, /*versionSize*/ 2,
    /*statusY*/ 34, /*statusSize*/ 3, /*dotSize*/ 18,
    /*countersY*/ 66, /*lineSize*/ 2,
    /*syncY*/ 88,
    /*huntY*/ 110, /*huntIndY*/ 128, /*huntBarY*/ 148, /*huntBarH*/ 4, /*indStep*/ 4 * kGlyphW * 2,
    /*bannerY*/ 110, /*bannerTitleSize*/ 2, /*bannerTitleY*/ 112, /*bannerEssidY*/ 132, /*bannerBssidY*/ 150,
};

// A board gets the roomy tier only when its panel is tall enough to lay the larger fonts out without
// clipping; otherwise the compact tier. Branches on panel capability, not board identity (ADR-0002);
// the threshold is a photo-tuned candidate (slice-0088). A future reader: do not restore a hardcoded
// 240/135 here — this renderer serves three panels at two resolutions.
constexpr int32_t kRoomyMinHeight = 160;

// The roomy banner's lowest line must stay on its design panel (the T-embed's 170 px); a nudge that
// overflows fails the build, not the panel at runtime (mirrors the direct renderer's static_assert).
constexpr int32_t kRoomyDesignH = 170;
static_assert(kRoomy.bannerBssidY + kRoomy.lineSize * kGlyphH <= kRoomyDesignH,
              "roomy banner's lowest line runs off the T-embed panel");

const Layout& layoutFor(int32_t height) { return height >= kRoomyMinHeight ? kRoomy : kCompact; }

uint16_t statusColor(ScreenStatus status) {
    switch (status) {
        case ScreenStatus::Hunting: return kGreen;
        case ScreenStatus::Working: return kBlue;
        case ScreenStatus::Degraded: return kAmber;
        case ScreenStatus::Fault: return kRed;
        case ScreenStatus::Booting: return kWhite;
    }
    return kWhite;
}

const char* statusWord(ScreenStatus status) {
    switch (status) {
        case ScreenStatus::Hunting: return "HUNTING";
        case ScreenStatus::Working: return "WORKING";
        case ScreenStatus::Degraded: return "DEGRADED";
        case ScreenStatus::Fault: return "FAULT";
        case ScreenStatus::Booting: return "BOOTING";
    }
    return "?";
}

/// How many of the four icon segments are filled at each level (ADR-0091's coarse bucket).
int32_t batterySegments(BatteryLevel level) {
    switch (level) {
        case BatteryLevel::Full: return 4;
        case BatteryLevel::High: return 3;
        case BatteryLevel::Mid: return 2;
        case BatteryLevel::Low: return 1;
        case BatteryLevel::Critical: return 0;
        case BatteryLevel::Unknown: return 0;
    }
    return 0;
}

/// The fill colour: green for a healthy charge, amber for Low (the warning band), red reserved for the
/// Critical outline (an empty icon drawn in alarm red).
uint16_t batteryFillColor(BatteryLevel level) { return level == BatteryLevel::Low ? kAmber : kGreen; }

}  // namespace

void Esp32ScreenRenderer::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color565) {
    for (int32_t yy = y; yy < y + h; ++yy) {
        for (int32_t xx = x; xx < x + w; ++xx) display_.drawPixel(xx, yy, color565);
    }
}

void Esp32ScreenRenderer::drawHud(const ScreenView& view) {
    const Layout& L = layoutFor(static_cast<int32_t>(display_.height()));

    // Title: the product name, then the firmware version drawn smaller after it (sourced from the one
    // kFirmwareVersion the splash uses — no magic version string, §4 #22's spirit). The version baseline
    // is dropped to sit on the name's baseline when the two differ in size.
    // The product wordmark: white glyphs over a banner-magenta drop shadow (slice-0097). The shared helper
    // owns the effect; the semantic HUD palette (incl. the high-contrast white CAPTURED/CRACKED banner) is
    // untouched.
    drawWordmark(display_, L.margin, L.titleY, kTitleName, L.titleSize);
    char version[16];
    std::snprintf(version, sizeof(version), "v%s", kFirmwareVersion);
    const int32_t versionX =
        L.margin + static_cast<int32_t>(std::strlen(kTitleName)) * kGlyphW * L.titleSize + kGlyphW * L.versionSize;
    const int32_t versionY = L.titleY + (L.titleSize - L.versionSize) * kGlyphH;
    display_.drawText(versionX, versionY, version, kDim, L.versionSize);

    // A filled square that blinks with the heartbeat: proof the loop is alive, like the LED's blink.
    if (view.heartbeat) fillRect(L.margin, L.statusY + kDotPadY, L.dotSize, L.dotSize, statusColor(view.status));
    display_.drawText(L.margin + L.dotSize + 6, L.statusY, statusWord(view.status), statusColor(view.status),
                      L.statusSize);

    char line[40];
    std::snprintf(line, sizeof(line), "up:%lu  cracks:%lu", static_cast<unsigned long>(view.uploaded),
                  static_cast<unsigned long>(view.cracks));
    display_.drawText(L.margin, L.countersY, line, kWhite, L.lineSize);

    if (!view.haveSynced) {
        display_.drawText(L.margin, L.syncY, "sync: --", kDim, L.lineSize);
    } else if (view.lastSyncOk) {
        std::snprintf(line, sizeof(line), "sync: ok +%lu", static_cast<unsigned long>(view.lastSyncNew));
        display_.drawText(L.margin, L.syncY, line, kGreen, L.lineSize);
    } else {
        display_.drawText(L.margin, L.syncY, "sync: FAIL", kAmber, L.lineSize);
    }
}

void Esp32ScreenRenderer::drawHuntLine(const ScreenView& view) {
    if (!view.huntShown) return;  // no hunt source wired: the pre-0033 HUD (ADR-0033 decision 2).

    const int32_t w = static_cast<int32_t>(display_.width());
    const Layout& L = layoutFor(static_cast<int32_t>(display_.height()));

    if (view.huntPhase == HuntPhase::Discovering) {
        char line[40];
        std::snprintf(line, sizeof(line), "SCAN ch%u  %lu seen", static_cast<unsigned>(view.huntChannel),
                      static_cast<unsigned long>(view.huntDiscovered));
        display_.drawText(L.margin, L.huntY, line, kCyan, L.lineSize);
        return;
    }
    if (view.huntPhase != HuntPhase::Capturing) {
        display_.drawText(L.margin, L.huntY, "idle", kDim, L.lineSize);
        return;
    }

    // Capturing: the target (SSID, or BSSID when hidden), then a Beacon/M1–M4 indicator row lit per
    // present frame, then a progress bar filled by how many of the five have arrived.
    char label[20];
    const char* name = view.huntSsid[0] != '\0' ? view.huntSsid : view.huntBssid;
    // Deliberately truncate to the field width so it fits beside the indicator row; the precision bound
    // (not a bare %s) is what tells the compiler the output is capped — no -Wformat-truncation.
    std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(sizeof(label) - 1), name);
    display_.drawText(L.margin, L.huntY, label, kWhite, L.lineSize);

    struct Indicator { const char* label; bool present; };
    const Indicator row[] = {{"B", view.huntHasBeacon}, {"1", view.huntHasM1}, {"2", view.huntHasM2},
                             {"3", view.huntHasM3}, {"4", view.huntHasM4}};
    // Compact shares the label line (indicators start after a ≤13-char size-1 label); roomy gives the
    // row its own line from the margin. Selected by whether the tier put huntIndY on the label line.
    const bool ownLine = L.huntIndY != L.huntY;
    int32_t x = ownLine ? L.margin : L.margin + 14 * kGlyphW;
    const int32_t step = ownLine ? L.indStep : 2 * kGlyphW;
    for (const Indicator& ind : row) {
        display_.drawText(x, L.huntIndY, ind.label, ind.present ? kGreen : kDim, L.lineSize);
        x += step;
    }

    const int32_t barW = w - 2 * L.margin;
    const uint8_t present = static_cast<uint8_t>(view.huntHasBeacon + view.huntHasM1 + view.huntHasM2 +
                                                 view.huntHasM3 + view.huntHasM4);
    fillRect(L.margin, L.huntBarY, barW, L.huntBarH, kDim);                    // track.
    fillRect(L.margin, L.huntBarY, barW * present / 5, L.huntBarH, kGreen);    // fill (present of 5).
}

void Esp32ScreenRenderer::drawBanner(const ScreenView& view) {
    // One banner slot, two labels (ADR-0031 #4): a capture is the frequent everyday event (blue title),
    // a crack the rare big win (red title) — the two are simply distinct; the LED picks its own capture
    // hue (cyan) independently. The renderer only labels; the surface decides which shows.
    const int32_t w = static_cast<int32_t>(display_.width());
    const int32_t h = static_cast<int32_t>(display_.height());
    const Layout& L = layoutFor(h);
    const bool cracked = view.toastKind == ToastKind::Cracked;
    fillRect(0, L.bannerY, w, h - L.bannerY, kWhite);
    display_.drawText(L.margin, L.bannerTitleY, cracked ? "* CRACKED *" : "* CAPTURED *",
                      cracked ? kRed : kBlue, L.bannerTitleSize);
    display_.drawText(L.margin, L.bannerEssidY, view.toastEssid, kBlack, L.lineSize);
    display_.drawText(L.margin, L.bannerBssidY, view.toastBssid, kBlack, L.lineSize);
}

void Esp32ScreenRenderer::drawBattery(const ScreenView& view) {
    if (!view.batteryShown) return;  // no calibrated gauge wired: no icon (ADR-0091).

    const int32_t w = static_cast<int32_t>(display_.width());
    const Layout& L = layoutFor(static_cast<int32_t>(display_.height()));

    // A small outlined battery with four segments, drawn top-right of the status row (the right side of
    // that row is clear on both tiers, where the title row carries the long "vX.Y.Z" string). Geometry
    // scales with the title size so it reads on the roomy panel; these are candidate values photo-tuned
    // on the panel (ADR-0002 §5), like the rest of the layout.
    const int32_t scale = L.titleSize >= 3 ? 2 : 1;
    const int32_t border = scale;
    const int32_t segW = 4 * scale;
    const int32_t segGap = 2 * scale;
    const int32_t innerPad = 2 * scale;
    constexpr int32_t kSegs = 4;
    const int32_t bodyW = kSegs * segW + (kSegs - 1) * segGap + 2 * innerPad + 2 * border;
    const int32_t bodyH = 11 * scale;
    const int32_t nubW = 2 * scale;
    const int32_t nubH = 4 * scale;
    // The body's right edge sits at the right margin; the terminal nub is on the LEFT of the body and the
    // fill is anchored to the right (a horizontal mirror of the phone-style icon, the requested orientation).
    const int32_t bodyX = w - L.margin - bodyW;
    // Vertically centred on the title row, so the icon sits in line with the product name top-right (not
    // dropped to the status row below it). The title glyph box is kGlyphH*titleSize tall.
    const int32_t y = L.titleY + (kGlyphH * L.titleSize - bodyH) / 2;

    const uint16_t outline = view.batteryLevel == BatteryLevel::Critical ? kRed : kWhite;
    fillRect(bodyX, y, bodyW, bodyH, outline);                                              // outer shell.
    fillRect(bodyX + border, y + border, bodyW - 2 * border, bodyH - 2 * border, kBlack);  // hollow it.
    fillRect(bodyX - nubW, y + (bodyH - nubH) / 2, nubW, nubH, outline);                    // terminal nub, on the left.

    const int32_t filled = batterySegments(view.batteryLevel);
    const uint16_t fill = batteryFillColor(view.batteryLevel);
    const int32_t segH = bodyH - 2 * border - 2 * innerPad;
    const int32_t segX0 = bodyX + border + innerPad;
    const int32_t segY = y + border + innerPad;
    // Fill the rightmost `filled` segments, anchored away from the left-hand nub.
    for (int32_t i = kSegs - filled; i < kSegs; ++i) {
        fillRect(segX0 + i * (segW + segGap), segY, segW, segH, fill);
    }
}

void Esp32ScreenRenderer::render(const ScreenView& view) {
    display_.fillScreen(kBlack);
    drawHud(view);
    drawBattery(view);   // top-right of the status row; a clear corner on both tiers.
    drawHuntLine(view);  // the live-hunt line sits above the banner; a banner overpaints its own strip.
    if (view.toastActive) drawBanner(view);
    display_.present();
}

}  // namespace sapper
