/**
 * @file screen_renderer_esp32.cpp
 * @brief Implementation of the device screen renderer (ADR-0025).
 */
#include "surface/screen_renderer_esp32.h"

#include <cstdio>
#include <cstring>

#include "config/firmware_version.h"  // kFirmwareVersion — the single version source the splash uses too.
#include "surface/glyph_metrics.h"     // kGlyphW/kGlyphH — shared 6x8 default-font box (rule-of-three, §3).
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

// kGlyphW/kGlyphH (the 6x8 default-font box) now come from surface/glyph_metrics.h; extracting them is
// byte-identical at runtime, so ADR-0086 decision 1's no-re-verification guarantee for this frozen renderer
// holds (only the definition site moved). They lay out rows and position the version string after the
// product name without a textWidth() the seam does not expose (the on-air photo is the proof, ADR-0025).
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
// One panel-size class laid out as the four ADR-0116 bands (header / status / body / footer). The three
// framing bands keep their role across both hunt modes; only the body swaps by HuntPhase. The banner is
// constrained to the body region ([bodyTop, bodyBottom)) so it never covers the header (the armed-TX
// chips, #6) or the status band. All fields are candidate values photo-tuned on the panel (ADR-0002 §5);
// the band roles are the invariant, not the numbers.
struct Layout {
    int32_t margin;
    // Header band.
    int32_t titleY;
    uint8_t titleSize;
    uint8_t versionSize;  ///< The "vX.Y.Z" string, drawn smaller after the product name.
    // Status band.
    int32_t statusY;
    uint8_t statusSize;
    int32_t dotSize;      ///< The heartbeat square beside the status word.
    uint8_t lineSize;     ///< Body + footer text (counters, sync, hunt label/indicators, PMKID badge).
    // Body band (the only band that swaps by phase); the banner clears [bodyTop, bodyBottom).
    int32_t bodyTop;
    int32_t huntY;        ///< SCAN line / idle / the capturing target label (its own full-width row, #3).
    int32_t huntIndY;     ///< The B/M1-M4 indicator row + the PMKID badge (its own row, #3/#4).
    int32_t huntBarY;
    int32_t huntBarH;
    int32_t indStep;      ///< Per-indicator advance.
    int32_t bodyBottom;
    // Footer band (at the bottom, below the body).
    int32_t footerCountersY;
    int32_t footerSyncY;
    // The CAPTURED/CRACKED banner, within the body region.
    uint8_t bannerTitleSize;
    int32_t bannerTitleY;
    int32_t bannerEssidY;
    int32_t bannerBssidY;
};

// Compact tier: 240x135 (Cardputer, M5StickC Plus2). Header y2..18, status y24..40, body y44..100 (the
// live centre), footer y108..130. The banner clears only [bodyTop=44, bodyBottom=100), so the armed-TX
// chips, battery, status word, RSSI glyph and footer stay visible under it (ADR-0116 #6). The target label
// (huntY) and the B/M1-M4 row (huntIndY) each own a full-width row (slice-0117 / ADR-0116 #3).
constexpr Layout kCompact{
    /*margin*/ 4,
    /*titleY*/ 2, /*titleSize*/ 2, /*versionSize*/ 1,
    /*statusY*/ 24, /*statusSize*/ 2, /*dotSize*/ 10,
    /*lineSize*/ 1,
    /*bodyTop*/ 44, /*huntY*/ 48, /*huntIndY*/ 62, /*huntBarY*/ 78, /*huntBarH*/ 2,
    /*indStep*/ 3 * kGlyphW, /*bodyBottom*/ 100,
    /*footerCountersY*/ 108, /*footerSyncY*/ 122,
    /*bannerTitleSize*/ 2, /*bannerTitleY*/ 48, /*bannerEssidY*/ 70, /*bannerBssidY*/ 84,
};

// Roomy tier: a taller/wider sprite panel (the LilyGo T-embed, 320x170). Larger fonts and the room the
// compact tier lacks. Header y4..28, status y32..56, body y60..126, footer y128..164. Same band roles.
constexpr Layout kRoomy{
    /*margin*/ 6,
    /*titleY*/ 4, /*titleSize*/ 3, /*versionSize*/ 2,
    /*statusY*/ 32, /*statusSize*/ 3, /*dotSize*/ 18,
    /*lineSize*/ 2,
    /*bodyTop*/ 60, /*huntY*/ 64, /*huntIndY*/ 88, /*huntBarY*/ 112, /*huntBarH*/ 4,
    /*indStep*/ 4 * kGlyphW * 2, /*bodyBottom*/ 126,
    /*footerCountersY*/ 128, /*footerSyncY*/ 148,
    /*bannerTitleSize*/ 2, /*bannerTitleY*/ 66, /*bannerEssidY*/ 90, /*bannerBssidY*/ 108,
};

// A board gets the roomy tier only when its panel is tall enough to lay the larger fonts out without
// clipping; otherwise the compact tier. Branches on panel capability, not board identity (ADR-0002);
// the threshold is a photo-tuned candidate (slice-0088). A future reader: do not restore a hardcoded
// 240/135 here — this renderer serves three panels at two resolutions.
constexpr int32_t kRoomyMinHeight = 160;

// The roomy banner's and footer's lowest lines must stay on their design panel (the T-embed's 170 px),
// and the banner (body region) must not overlap the footer; a nudge that overflows fails the build, not
// the panel at runtime (mirrors the direct renderer's static_asserts).
constexpr int32_t kRoomyDesignH = 170;
static_assert(kRoomy.bannerBssidY + kRoomy.lineSize * kGlyphH <= kRoomy.bodyBottom,
              "roomy banner's lowest line runs past the body region into the footer");
static_assert(kRoomy.bodyBottom <= kRoomy.footerCountersY, "roomy body region overlaps the footer");
static_assert(kRoomy.footerSyncY + kRoomy.lineSize * kGlyphH <= kRoomyDesignH,
              "roomy footer's lowest line runs off the T-embed panel");
static_assert(kCompact.bannerBssidY + kCompact.lineSize * kGlyphH <= kCompact.bodyBottom,
              "compact banner's lowest line runs past the body region into the footer");
static_assert(kCompact.bodyBottom <= kCompact.footerCountersY, "compact body region overlaps the footer");
static_assert(kCompact.footerSyncY + kCompact.lineSize * kGlyphH <= 135,
              "compact footer's lowest line runs off the 240x135 panel");

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

/// How many of the four signal bars are lit at each coarse bucket (ADR-0116 #5). Unknown lights none,
/// which drawRssi then renders as a hidden glyph (not four empty bars) so an unknown signal reads as absent.
int32_t rssiBars(RssiLevel level) {
    switch (level) {
        case RssiLevel::Strong: return 4;
        case RssiLevel::Good: return 3;
        case RssiLevel::Fair: return 2;
        case RssiLevel::Weak: return 1;
        case RssiLevel::Unknown: return 0;
    }
    return 0;
}

/// The battery icon's left-most pixel (the nub edge), so the armed-TX chips can anchor just left of it and
/// the icon's own body position derives from the same formula (one width definition for both — §3 DRY).
int32_t batteryLeftEdge(const Layout& L, int32_t w) {
    const int32_t scale = L.titleSize >= 3 ? 2 : 1;
    const int32_t border = scale;
    const int32_t segW = 4 * scale;
    const int32_t segGap = 2 * scale;
    const int32_t innerPad = 2 * scale;
    const int32_t nubW = 2 * scale;
    const int32_t bodyW = 4 * segW + 3 * segGap + 2 * innerPad + 2 * border;  // 4 segments (kSegs).
    return w - L.margin - bodyW - nubW;
}

}  // namespace

void Esp32ScreenRenderer::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color565) {
    for (int32_t yy = y; yy < y + h; ++yy) {
        for (int32_t xx = x; xx < x + w; ++xx) display_.drawPixel(xx, yy, color565);
    }
}

void Esp32ScreenRenderer::drawHeader(const ScreenView& view) {
    (void)view;  // the header's variable content (battery, armed chips) draws in its own functions.
    const Layout& L = layoutFor(static_cast<int32_t>(display_.height()));

    // Title: the product name, then the firmware version drawn smaller after it (sourced from the one
    // kFirmwareVersion the splash uses — no magic version string, §4 #22's spirit). The version baseline
    // is dropped to sit on the name's baseline when the two differ in size. The product wordmark: white
    // glyphs over a banner-magenta drop shadow (slice-0097); the shared helper owns the effect.
    drawWordmark(display_, L.margin, L.titleY, kTitleName, L.titleSize);
    char version[16];
    std::snprintf(version, sizeof(version), "v%s", kFirmwareVersion);
    const int32_t versionX =
        L.margin + static_cast<int32_t>(std::strlen(kTitleName)) * kGlyphW * L.titleSize + kGlyphW * L.versionSize;
    const int32_t versionY = L.titleY + (L.titleSize - L.versionSize) * kGlyphH;
    display_.drawText(versionX, versionY, version, kDim, L.versionSize);
}

void Esp32ScreenRenderer::drawStatusBand(const ScreenView& view) {
    const Layout& L = layoutFor(static_cast<int32_t>(display_.height()));
    // A filled square that blinks with the heartbeat: proof the loop is alive, like the LED's blink. The
    // status word is health/phase only (ADR-0116 #2), never the activity the body shows.
    if (view.heartbeat) fillRect(L.margin, L.statusY + kDotPadY, L.dotSize, L.dotSize, statusColor(view.status));
    display_.drawText(L.margin + L.dotSize + 6, L.statusY, statusWord(view.status), statusColor(view.status),
                      L.statusSize);
    drawRssi(view);  // the target signal glyph sits at the right of the status band (ADR-0116 #5).
}

void Esp32ScreenRenderer::drawFooter(const ScreenView& view) {
    const Layout& L = layoutFor(static_cast<int32_t>(display_.height()));
    // Reference counters at the bottom (ADR-0116 #1): this run's uploads, and the LIFETIME cracked total
    // read from the persisted manifest (ADR-0118), which no longer resets on reboot.
    char line[40];
    std::snprintf(line, sizeof(line), "up:%lu  cracked:%lu", static_cast<unsigned long>(view.uploaded),
                  static_cast<unsigned long>(view.crackedTotal));
    display_.drawText(L.margin, L.footerCountersY, line, kWhite, L.lineSize);

    if (!view.haveSynced) {
        display_.drawText(L.margin, L.footerSyncY, "sync: --", kDim, L.lineSize);
    } else if (view.lastSyncOk) {
        std::snprintf(line, sizeof(line), "sync: ok +%lu", static_cast<unsigned long>(view.lastSyncNew));
        display_.drawText(L.margin, L.footerSyncY, line, kGreen, L.lineSize);
    } else {
        display_.drawText(L.margin, L.footerSyncY, "sync: FAIL", kAmber, L.lineSize);
    }
}

void Esp32ScreenRenderer::drawRssi(const ScreenView& view) {
    // The target's signal is a capturing-target datum; show it only while Capturing, and hide the glyph
    // (not four empty bars) when the bucket is Unknown so an unrecorded signal reads as absent, not weak.
    if (!view.huntShown || view.huntPhase != HuntPhase::Capturing || view.huntRssi == RssiLevel::Unknown) return;
    const int32_t w = static_cast<int32_t>(display_.width());
    const Layout& L = layoutFor(static_cast<int32_t>(display_.height()));
    const int32_t scale = L.statusSize >= 3 ? 2 : 1;
    const int32_t barW = 3 * scale;
    const int32_t gap = 2 * scale;
    constexpr int32_t kBars = 4;
    const int32_t x0 = w - L.margin - (kBars * barW + (kBars - 1) * gap);  // right-aligned in the status band.
    const int32_t baseline = L.statusY + L.statusSize * kGlyphH;           // sit the bars on the status text's bottom.
    const int32_t lit = rssiBars(view.huntRssi);
    for (int32_t i = 0; i < kBars; ++i) {
        const int32_t h = (3 + i * 3) * scale;  // rising bars.
        fillRect(x0 + i * (barW + gap), baseline - h, barW, h, i < lit ? kGreen : kDim);
    }
}

void Esp32ScreenRenderer::drawArmedTx(const ScreenView& view) {
    // Colour carries "this unit is transmitting, and I am responsible" (ADR-0116 #6); the letter only
    // disambiguates which arm. Hidden entirely when passive, so a non-transmitting unit shows nothing.
    const int32_t count = (view.armedDeauth ? 1 : 0) + (view.armedSolicit ? 1 : 0);
    if (count == 0) return;
    const int32_t w = static_cast<int32_t>(display_.width());
    const Layout& L = layoutFor(static_cast<int32_t>(display_.height()));
    const int32_t scale = L.titleSize >= 3 ? 2 : 1;
    const int32_t pad = 2 * scale;
    const int32_t gap = 2 * scale;
    const int32_t chipW = kGlyphW * L.lineSize + 2 * pad;
    const int32_t chipH = kGlyphH * L.lineSize + 2 * pad;
    const int32_t chipY = L.titleY + (kGlyphH * L.titleSize - chipH) / 2;  // centred on the title row, like the battery.
    const int32_t cluster = count * chipW + (count - 1) * gap;
    int32_t x = batteryLeftEdge(L, w) - gap - cluster;  // the chip cluster hugs the left of the battery zone.
    const auto chip = [&](const char* letter, uint16_t color) {
        fillRect(x, chipY, chipW, chipH, color);
        display_.drawText(x + pad, chipY + pad, letter, kBlack, L.lineSize);
        x += chipW + gap;
    };
    if (view.armedDeauth) chip("D", kRed);      // deauth: the jamming arm, the stronger alarm hue.
    if (view.armedSolicit) chip("P", kAmber);   // PMKID solicitation: the station-impersonation arm.
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

    // Capturing: the target (SSID, or BSSID when hidden) on its own full-width row, then a
    // Beacon/M1-M4 indicator row (its own row, slice-0117 / ADR-0116 #3) lit per present frame, then a
    // progress bar filled by how many of the five have arrived.
    char label[40];
    const char* name = view.huntSsid[0] != '\0' ? view.huntSsid : view.huntBssid;
    // Clamp to the characters that fit the dedicated row at this tier's font size, so a long name never
    // overflows the panel; the %.*s precision (not a bare %s) also caps the output for the compiler (no
    // -Wformat-truncation). A 32-char SSID shows in full on the 240px compact row; snprintf's sizeof
    // bound keeps the buffer safe regardless of the computed fit.
    const int32_t fit = (w - 2 * L.margin) / (kGlyphW * L.lineSize);
    std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(fit), name);
    display_.drawText(L.margin, L.huntY, label, kWhite, L.lineSize);

    struct Indicator { const char* label; bool present; };
    const Indicator row[] = {{"B", view.huntHasBeacon}, {"M1", view.huntHasM1}, {"M2", view.huntHasM2},
                             {"M3", view.huntHasM3}, {"M4", view.huntHasM4}};
    // Every tier gives the indicator row its own line (slice-0117 / ADR-0116 #3), so the indicators always
    // start at the margin and advance by the tier's step.
    int32_t x = L.margin;
    for (const Indicator& ind : row) {
        display_.drawText(x, L.huntIndY, ind.label, ind.present ? kGreen : kDim, L.lineSize);
        x += L.indStep;
    }
    // The PMKID badge, set apart at the right of the indicator row (ADR-0116 #4): a clientless PMKID is an
    // alternative terminal extracted from M1 (ADR-0110), NOT a sixth frame of the four-way ladder, so it is
    // a distinct badge well clear of the B/M1-M4 cluster. Lit green on the clientless path, dim otherwise,
    // so the slot is a stable landmark the eye learns rather than a shifting popup.
    static constexpr char kPmkidLabel[] = "PMKID";
    const int32_t pmkW = static_cast<int32_t>(std::strlen(kPmkidLabel)) * kGlyphW * L.lineSize;
    display_.drawText(w - L.margin - pmkW, L.huntIndY, kPmkidLabel, view.huntHasPmkid ? kGreen : kDim, L.lineSize);

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
    const Layout& L = layoutFor(static_cast<int32_t>(display_.height()));
    const bool cracked = view.toastKind == ToastKind::Cracked;
    // Constrained to the body region only (ADR-0116 #6): the header (armed-TX chips, battery), the status
    // band, and the footer stay visible, so a congratulatory banner can never hide the safety indicator.
    fillRect(0, L.bodyTop, w, L.bodyBottom - L.bodyTop, kWhite);
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
    // bodyX is derived from the shared batteryLeftEdge() so the icon and the armed-TX chips agree on one
    // width definition (§3 DRY): leftEdge is the nub's left pixel, so the body starts nubW to its right.
    const int32_t bodyX = batteryLeftEdge(L, w) + nubW;
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
    drawHeader(view);       // band 1: wordmark + version.
    drawBattery(view);      // header, top-right corner.
    drawArmedTx(view);      // header, left of the battery (the armed-TX chips, #6).
    drawStatusBand(view);   // band 2: heartbeat + health/phase word + RSSI bars.
    drawHuntLine(view);     // band 3 (body): the SCAN line, or the target + indicators + PMKID + bar.
    drawFooter(view);       // band 4: counters + sync, at the bottom.
    // The banner overlays the body region ONLY (#6), drawn last so it sits over the body; the header,
    // status, and footer bands stay visible beneath it.
    if (view.toastActive) drawBanner(view);
    display_.present();
}

}  // namespace sapper
