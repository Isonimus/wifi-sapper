/**
 * @file fake_screen_renderer.h
 * @brief A test ScreenRenderer that records the views it is shown (ADR-0025).
 *
 * The surface renders only when the view changes, so `views` is exactly the sequence of distinct
 * frames — the observable behaviour the screen policy tests assert against, without any pixels.
 */
#pragma once

#include <vector>

#include "surface/screen_renderer.h"

namespace sapper_test {

class FakeScreenRenderer : public sapper::ScreenRenderer {
public:
    std::vector<sapper::ScreenView> views;

    void render(const sapper::ScreenView& view) override { views.push_back(view); }

    /// The most recently rendered view (an empty default before the first render).
    const sapper::ScreenView& last() const {
        static const sapper::ScreenView empty;
        return views.empty() ? empty : views.back();
    }
    size_t renderCount() const { return views.size(); }
};

}  // namespace sapper_test
