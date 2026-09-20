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
constexpr uint16_t kDim = 0x8410;   // grey for a not-yet-happened field (e.g. no sync yet).

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
    if (view.toastActive) drawBanner(view);
    display_.present();
}

}  // namespace sapper
