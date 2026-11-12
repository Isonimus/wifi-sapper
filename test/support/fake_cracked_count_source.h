/**
 * @file fake_cracked_count_source.h
 * @brief A CrackedCountSource double for the host lane (ADR-0118).
 *
 * Lets a test drive the HUD's lifetime-cracked counter without a real CrackedManifest: set `count` to the
 * total the scenario needs, and the surface pulls it each render exactly as it pulls the real manifest.
 */
#pragma once

#include <cstddef>

#include "net/cracked_count_source.h"

namespace sapper_test {

class FakeCrackedCountSource : public sapper::CrackedCountSource {
public:
    size_t count = 0;
    size_t crackedTotal() const override { return count; }
};

}  // namespace sapper_test
