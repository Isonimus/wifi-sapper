/**
 * @file direct_panel_screen_renderer.cpp
 * @brief Implementation of the direct-panel screen renderer: 480x320 layout + region-repaint (ADR-0086).
 */
#include "surface/direct_panel_screen_renderer.h"

// Compiles to nothing unless this board selects the direct renderer (ADR-0086 / §4 #39): on a sprite board
// ActiveScreenRenderer is Esp32ScreenRenderer and this TU is unused, so guarding it keeps the sprite boards
// byte-for-byte (its string literals would otherwise linger as dead rodata the linker does not strip),
// mirroring direct_panel_display.cpp's capability guard.
#if defined(SAPPER_DISPLAY_DIRECT_PANEL)

#include <cstdio>
#include <cstring>

#include "config/firmware_version.h"  // kFirmwareVersion — the single version source the splash + sprite HUD use.
#include "surface/glyph_metrics.h"     // kGlyphW/kGlyphH — shared 6x8 default-font box (rule-of-three, §3).
#include "surface/wordmark.h"          // drawWordmark — the shared product wordmark (slice-0097).

namespace sapper {
namespace {

// RGB565 palette. Deliberately duplicated from Esp32ScreenRenderer (and already from panel_probe): this is
// the second ScreenView->glyph renderer, where rule-of-three says duplicate not abstract (ADR-0086 decision
// 4 / §3). The status hues match the LED's intent (green hunting, blue working, amber degraded, red fault).
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kBlue = 0x001F;
constexpr uint16_t kAmber = 0xFD20;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kCyan = 0x07FF;  // the live-hunt SCAN line (ADR-0033).
constexpr uint16_t kDim = 0x8410;   // grey for a not-yet-happened field (no sync yet, an absent M-frame).

// Layout for the 480x320 landscape panel (rotation 1, slice-0085). Candidate values, photo-tuned on the
// physical panel (ADR-0002 §5 / ADR-0086 decision 5); the region-repaint logic is the invariant, not these.
// kGlyphW/kGlyphH (the 6x8 default-font box) come from surface/glyph_metrics.h; extracting them is
// byte-identical at runtime, so ADR-0086 decision 1's no-re-verification guarantee for this frozen renderer holds.
constexpr int32_t kMargin = 10;

constexpr int32_t kTitleY = 12;
constexpr uint8_t kTitleSize = 4;
constexpr uint8_t kVersionSize = 2;  ///< The "vX.Y.Z" string, drawn smaller after the product name (slice-0088).
const char kTitleName[] = "WiFi Sapper";

constexpr int32_t kStatusY = 64;
constexpr uint8_t kStatusSize = 3;
constexpr int32_t kDotSize = 26;             // the heartbeat square beside the status word.
constexpr int32_t kStatusTextX = kMargin + kDotSize + 12;

constexpr int32_t kCountersY = 120;
constexpr int32_t kSyncY = 152;
constexpr uint8_t kLineSize = 2;

// The live-hunt band (ADR-0033): a phase line, an indicator row (when Capturing), and a progress bar. Cleared
// and redrawn as a unit when any hunt field changes (a local band repaint, never a whole-panel clear).
constexpr int32_t kHuntBandTop = 184;
constexpr int32_t kHuntY = 190;              // SCAN line / idle / the capturing target label.
constexpr int32_t kHuntIndY = 214;           // the B/M1-M4 indicator row (Capturing only).
constexpr int32_t kHuntBarY = 238;
constexpr int32_t kHuntBarH = 8;
constexpr int32_t kIndStep = 4 * kGlyphW * kLineSize;  // per-indicator advance: one glyph + airy spacing.

// The CAPTURED/CRACKED banner gets its own lower strip below the HUD (room the 240x135 panel lacked, ADR-0086
// decision 3), so no HUD field sits under it: arming/clearing it repaints only this strip. Three lines (a
// size-kStatusSize title + two size-kLineSize lines) must fit inside the 64 px strip 256..319.
constexpr int32_t kDesignPanelH = 320;                 // the 480x320 panel this renderer targets (slice-0085).
constexpr int32_t kBannerStripY = 256;
constexpr int32_t kBannerTitleY = kBannerStripY + 4;   // size kStatusSize (24 px high): rows 260..283.
constexpr int32_t kBannerEssidY = kBannerStripY + 28;  // size kLineSize   (16 px high): rows 284..299.
constexpr int32_t kBannerBssidY = kBannerStripY + 46;  // size kLineSize   (16 px high): rows 302..317, on-panel.

// Compile-time layout budget: the banner's lowest line must stay on the panel, and the strip must sit below
// the hunt band. A future nudge that overflows these fails the build, not the panel at runtime (an off-panel
// row address can wrap into visible corruption on an ST7796, not just clip). The region-repaint logic is the
// invariant; the exact coordinates are photo-tuned (ADR-0086 decision 5), but only within this budget.
static_assert(kBannerBssidY + kLineSize * kGlyphH <= kDesignPanelH, "banner's lowest line runs off the panel");
static_assert(kBannerStripY >= kHuntBarY + kHuntBarH, "banner strip overlaps the hunt band");

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

bool huntFieldsDiffer(const ScreenView& a, const ScreenView& b) {
    return a.huntShown != b.huntShown || a.huntPhase != b.huntPhase || a.huntChannel != b.huntChannel ||
           a.huntDiscovered != b.huntDiscovered || a.huntHasBeacon != b.huntHasBeacon ||
           a.huntHasM1 != b.huntHasM1 || a.huntHasM2 != b.huntHasM2 || a.huntHasM3 != b.huntHasM3 ||
           a.huntHasM4 != b.huntHasM4 || std::strcmp(a.huntSsid, b.huntSsid) != 0 ||
           std::strcmp(a.huntBssid, b.huntBssid) != 0;
}

}  // namespace

void DirectPanelScreenRenderer::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color565) {
    for (int32_t yy = y; yy < y + h; ++yy) {
        for (int32_t xx = x; xx < x + w; ++xx) display_.drawPixel(xx, yy, color565);
    }
}

void DirectPanelScreenRenderer::drawChromeOnce() {
    // The product title never changes, so it is drawn once and never repainted (ADR-0086 decision 3). The
    // firmware version is drawn smaller after the name, from the one kFirmwareVersion the splash uses (no
    // magic version string, §4 #22's spirit; slice-0088). Its baseline is dropped to sit on the name's.
    // The product wordmark: white glyphs over a banner-magenta drop shadow (slice-0097). The shared helper
    // owns the effect; the semantic HUD palette (incl. the high-contrast white CAPTURED/CRACKED banner) is
    // untouched.
    drawWordmark(display_, kMargin, kTitleY, kTitleName, kTitleSize);
    char version[16];
    std::snprintf(version, sizeof(version), "v%s", kFirmwareVersion);
    const int32_t versionX =
        kMargin + static_cast<int32_t>(std::strlen(kTitleName)) * kGlyphW * kTitleSize + kGlyphW * kVersionSize;
    const int32_t versionY = kTitleY + (kTitleSize - kVersionSize) * kGlyphH;
    display_.drawText(versionX, versionY, version, kDim, kVersionSize);
}

void DirectPanelScreenRenderer::drawStatus(const ScreenView& view) {
    const bool first = !chromeDrawn_;
    const bool statusChanged = first || view.status != last_.status;
    const bool heartbeatChanged = first || view.heartbeat != last_.heartbeat;

    // The heartbeat square: lit in the status colour when on, cleared when off. Repaint it when the heartbeat
    // toggled, or when its colour (the status) changed while lit. This is the only field a pure heartbeat tick
    // touches, so an idle HUD shows no whole-panel flash (the reason region-repaint exists, ADR-0086).
    if (heartbeatChanged || (view.heartbeat && statusChanged)) {
        fillRect(kMargin, kStatusY, kDotSize, kDotSize, view.heartbeat ? statusColor(view.status) : kBlack);
    }
    if (statusChanged) {
        const int32_t w = static_cast<int32_t>(display_.width()) - kStatusTextX - kMargin;
        fillRect(kStatusTextX, kStatusY, w, kStatusSize * kGlyphH, kBlack);  // clear a now-shorter word.
        display_.drawText(kStatusTextX, kStatusY, statusWord(view.status), statusColor(view.status), kStatusSize);
    }
}

void DirectPanelScreenRenderer::drawCounters(const ScreenView& view) {
    const bool first = !chromeDrawn_;
    if (!first && view.uploaded == last_.uploaded && view.cracks == last_.cracks) return;
    char line[40];
    std::snprintf(line, sizeof(line), "up:%lu  cracks:%lu", static_cast<unsigned long>(view.uploaded),
                  static_cast<unsigned long>(view.cracks));
    fillRect(kMargin, kCountersY, static_cast<int32_t>(display_.width()) - 2 * kMargin, kLineSize * kGlyphH, kBlack);
    display_.drawText(kMargin, kCountersY, line, kWhite, kLineSize);
}

void DirectPanelScreenRenderer::drawSync(const ScreenView& view) {
    const bool first = !chromeDrawn_;
    if (!first && view.haveSynced == last_.haveSynced && view.lastSyncOk == last_.lastSyncOk &&
        view.lastSyncNew == last_.lastSyncNew) {
        return;
    }
    char line[40];
    uint16_t color = kDim;
    if (!view.haveSynced) {
        std::snprintf(line, sizeof(line), "sync: --");
    } else if (view.lastSyncOk) {
        std::snprintf(line, sizeof(line), "sync: ok +%lu", static_cast<unsigned long>(view.lastSyncNew));
        color = kGreen;
    } else {
        std::snprintf(line, sizeof(line), "sync: FAIL");
        color = kAmber;
    }
    fillRect(kMargin, kSyncY, static_cast<int32_t>(display_.width()) - 2 * kMargin, kLineSize * kGlyphH, kBlack);
    display_.drawText(kMargin, kSyncY, line, color, kLineSize);
}

void DirectPanelScreenRenderer::drawHuntLine(const ScreenView& view) {
    const bool first = !chromeDrawn_;
    if (!first && !huntFieldsDiffer(view, last_)) return;

    // Clear the whole hunt band once (label + indicator row + bar) and redraw it for the current phase: a
    // local band repaint, not a whole-panel clear, and only when a hunt field moved.
    fillRect(0, kHuntBandTop, static_cast<int32_t>(display_.width()), kBannerStripY - kHuntBandTop, kBlack);
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

    // Capturing: the target (SSID, or BSSID when hidden), a Beacon/M1-M4 indicator row lit per present frame,
    // and a progress bar filled by how many of the five have arrived.
    const char* name = view.huntSsid[0] != '\0' ? view.huntSsid : view.huntBssid;
    char label[20];
    std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(sizeof(label) - 1), name);
    display_.drawText(kMargin, kHuntY, label, kWhite, kLineSize);

    struct Indicator { const char* label; bool present; };
    const Indicator row[] = {{"B", view.huntHasBeacon}, {"1", view.huntHasM1}, {"2", view.huntHasM2},
                             {"3", view.huntHasM3}, {"4", view.huntHasM4}};
    int32_t x = kMargin;
    for (const Indicator& ind : row) {
        display_.drawText(x, kHuntIndY, ind.label, ind.present ? kGreen : kDim, kLineSize);
        x += kIndStep;
    }

    const int32_t barW = static_cast<int32_t>(display_.width()) - 2 * kMargin;
    const uint8_t present = static_cast<uint8_t>(view.huntHasBeacon + view.huntHasM1 + view.huntHasM2 +
                                                 view.huntHasM3 + view.huntHasM4);
    fillRect(kMargin, kHuntBarY, barW, kHuntBarH, kDim);                  // track.
    fillRect(kMargin, kHuntBarY, barW * present / 5, kHuntBarH, kGreen);  // fill (present of 5).
}

void DirectPanelScreenRenderer::drawBanner(const ScreenView& view) {
    const bool first = !chromeDrawn_;
    const bool contentChanged = view.toastKind != last_.toastKind ||
                                std::strcmp(view.toastEssid, last_.toastEssid) != 0 ||
                                std::strcmp(view.toastBssid, last_.toastBssid) != 0;
    const bool changed = first || view.toastActive != last_.toastActive || (view.toastActive && contentChanged);
    if (!changed) return;

    const int32_t w = static_cast<int32_t>(display_.width());
    const int32_t h = static_cast<int32_t>(display_.height()) - kBannerStripY;
    if (!view.toastActive) {
        // The strip is already black on the first render (from the initial fillScreen); otherwise clear it.
        if (!first) fillRect(0, kBannerStripY, w, h, kBlack);
        return;
    }

    // One slot, two labels (ADR-0031 #4): a capture (blue) is the everyday event, a crack (red) the big win.
    const bool cracked = view.toastKind == ToastKind::Cracked;
    fillRect(0, kBannerStripY, w, h, kWhite);
    display_.drawText(kMargin, kBannerTitleY, cracked ? "* CRACKED *" : "* CAPTURED *",
                      cracked ? kRed : kBlue, kStatusSize);
    display_.drawText(kMargin, kBannerEssidY, view.toastEssid, kBlack, kLineSize);
    display_.drawText(kMargin, kBannerBssidY, view.toastBssid, kBlack, kLineSize);
}

void DirectPanelScreenRenderer::render(const ScreenView& view) {
    if (!chromeDrawn_) {
        display_.fillScreen(kBlack);  // the one whole-panel clear, on the first render only.
        drawChromeOnce();
    }
    // Each field repaints only when it changed vs last_ (or on the first render, while chromeDrawn_ is still
    // false). No whole-panel clear here: that is the ~1 Hz heartbeat flash region-repaint exists to avoid.
    drawStatus(view);
    drawCounters(view);
    drawSync(view);
    drawHuntLine(view);
    drawBanner(view);

    chromeDrawn_ = true;
    last_ = view;
    display_.present();  // no-op on a direct panel (the writes already reached the glass, ADR-0084).
}

}  // namespace sapper

#endif  // SAPPER_DISPLAY_DIRECT_PANEL
