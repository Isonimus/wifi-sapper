/**
 * @file screen_renderer_esp32.cpp
 * @brief Implementation of the device screen renderer (ADR-0025).
 */
#include "surface/screen_renderer_esp32.h"

#include <cstdio>

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

// Layout, in pixels, for the 240x135 panel (ADR-0002 §5). Named so a nudge is not a magic-number hunt.
constexpr int32_t kMargin = 4;
constexpr int32_t kTitleY = 2;
constexpr int32_t kStatusY = 26;
constexpr int32_t kCountersY = 52;
constexpr int32_t kSyncY = 68;
constexpr uint8_t kTitleSize = 2;
constexpr uint8_t kStatusSize = 2;
constexpr uint8_t kLineSize = 1;
constexpr int32_t kDotSize = 10;    // the heartbeat square beside the status word.

// The live-hunt line sits between the sync line and the banner (ADR-0033): a text row plus a thin
// progress bar just above the banner strip. size-1 glyphs advance ~6 px each — good enough to lay out
// the indicator row without a textWidth() the seam does not expose (the on-air PNG is the eyeball proof).
constexpr int32_t kHuntY = 76;
constexpr int32_t kHuntBarY = 84;
constexpr int32_t kHuntBarH = 2;
constexpr int32_t kGlyphW = 6;

// The CRACKED banner occupies the lower strip, over the HUD.
constexpr int32_t kBannerY = 86;
constexpr int32_t kBannerH = 135 - kBannerY;
constexpr int32_t kBannerTitleY = kBannerY + 4;
constexpr int32_t kBannerEssidY = kBannerY + 24;
constexpr int32_t kBannerBssidY = kBannerY + 36;

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

}  // namespace

void Esp32ScreenRenderer::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color565) {
    for (int32_t yy = y; yy < y + h; ++yy) {
        for (int32_t xx = x; xx < x + w; ++xx) display_.drawPixel(xx, yy, color565);
    }
}

void Esp32ScreenRenderer::drawHud(const ScreenView& view) {
    display_.drawText(kMargin, kTitleY, "WiFi Sapper", kWhite, kTitleSize);

    // A filled square that blinks with the heartbeat: proof the loop is alive, like the LED's blink.
    if (view.heartbeat) fillRect(kMargin, kStatusY + 2, kDotSize, kDotSize, statusColor(view.status));
    display_.drawText(kMargin + kDotSize + 6, kStatusY, statusWord(view.status), statusColor(view.status),
                      kStatusSize);

    char line[40];
    std::snprintf(line, sizeof(line), "up:%lu  cracks:%lu", static_cast<unsigned long>(view.uploaded),
                  static_cast<unsigned long>(view.cracks));
    display_.drawText(kMargin, kCountersY, line, kWhite, kLineSize);

    if (!view.haveSynced) {
        display_.drawText(kMargin, kSyncY, "sync: --", kDim, kLineSize);
    } else if (view.lastSyncOk) {
        std::snprintf(line, sizeof(line), "sync: ok +%lu", static_cast<unsigned long>(view.lastSyncNew));
        display_.drawText(kMargin, kSyncY, line, kGreen, kLineSize);
    } else {
        display_.drawText(kMargin, kSyncY, "sync: FAIL", kAmber, kLineSize);
    }
}

void Esp32ScreenRenderer::drawHuntLine(const ScreenView& view) {
    if (!view.huntShown) return;  // no hunt source wired: the pre-0033 HUD (ADR-0033 decision 2).

    if (view.huntPhase == HuntPhase::Discovering) {
        char line[40];
        std::snprintf(line, sizeof(line), "SCAN ch%u  %lu seen", static_cast<unsigned>(view.huntChannel),
                      static_cast<unsigned long>(view.huntDiscovered));
        display_.drawText(kMargin, kHuntY, line, kCyan, kLineSize);
        return;
    }
    if (view.huntPhase != HuntPhase::Capturing) {
        display_.drawText(kMargin, kHuntY, "idle", kDim, kLineSize);
        return;
    }

    // Capturing: the target (SSID, or BSSID when hidden), then a Beacon/M1–M4 indicator row lit per
    // present frame, then a progress bar filled by how many of the five have arrived.
    char label[14];
    const char* name = view.huntSsid[0] != '\0' ? view.huntSsid : view.huntBssid;
    // Deliberately truncate to the field width so it fits beside the indicator row; the precision bound
    // (not a bare %s) is what tells the compiler the output is capped — no -Wformat-truncation.
    std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(sizeof(label) - 1), name);
    display_.drawText(kMargin, kHuntY, label, kWhite, kLineSize);

    struct Indicator { const char* label; bool present; };
    const Indicator row[] = {{"B", view.huntHasBeacon}, {"1", view.huntHasM1}, {"2", view.huntHasM2},
                             {"3", view.huntHasM3}, {"4", view.huntHasM4}};
    int32_t x = kMargin + 14 * kGlyphW;  // after the (≤13-char) label + a gap.
    for (const Indicator& ind : row) {
        display_.drawText(x, kHuntY, ind.label, ind.present ? kGreen : kDim, kLineSize);
        x += 2 * kGlyphW;  // one glyph + a space.
    }

    const int32_t barW = 240 - 2 * kMargin;
    const uint8_t present = static_cast<uint8_t>(view.huntHasBeacon + view.huntHasM1 + view.huntHasM2 +
                                                 view.huntHasM3 + view.huntHasM4);
    fillRect(kMargin, kHuntBarY, barW, kHuntBarH, kDim);                    // track.
    fillRect(kMargin, kHuntBarY, barW * present / 5, kHuntBarH, kGreen);    // fill (present of 5).
}

void Esp32ScreenRenderer::drawBanner(const ScreenView& view) {
    // One banner slot, two labels (ADR-0031 #4): a capture is the frequent everyday event (blue title),
    // a crack the rare big win (red title) — the two are simply distinct; the LED picks its own capture
    // hue (cyan) independently. The renderer only labels; the surface decides which shows.
    const bool cracked = view.toastKind == ToastKind::Cracked;
    fillRect(0, kBannerY, 240, kBannerH, kWhite);
    display_.drawText(kMargin, kBannerTitleY, cracked ? "* CRACKED *" : "* CAPTURED *",
                      cracked ? kRed : kBlue, kStatusSize);
    display_.drawText(kMargin, kBannerEssidY, view.toastEssid, kBlack, kLineSize);
    display_.drawText(kMargin, kBannerBssidY, view.toastBssid, kBlack, kLineSize);
}

void Esp32ScreenRenderer::render(const ScreenView& view) {
    display_.fillScreen(kBlack);
    drawHud(view);
    drawHuntLine(view);  // the live-hunt line sits above the banner; a banner (86+) overpaints its own strip.
    if (view.toastActive) drawBanner(view);
    display_.present();
}

}  // namespace sapper
