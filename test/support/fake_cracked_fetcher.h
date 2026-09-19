/**
 * @file fake_cracked_fetcher.h
 * @brief Scripted CrackedResultsFetcher double for the host lane (ADR-0019 decision #1; slice-0020).
 *
 * Replays a fixed list of body lines to the sink and returns a scripted FetchResult, so the sync's
 * fetch→parse→mirror→alert policy is proved without a network: a captured `?api&dl=1` body drives the
 * success paths, and a Transport result with no lines (or an all-garbage body) drives the fail-loud
 * paths (Scenario I). It records the key it was called with so a test can assert the cookie wiring.
 */
#pragma once

#include <string>
#include <vector>

#include "net/cracked_fetcher.h"

namespace sapper_test {

class FakeCrackedFetcher : public sapper::CrackedResultsFetcher {
public:
    sapper::FetchResult result = sapper::FetchResult::Ok;  ///< What fetch() returns.
    std::vector<std::string> lines;                        ///< Body lines yielded when result == Ok.
    std::string lastKey;                                   ///< The key fetch() was called with.
    int fetchCount = 0;

    sapper::FetchResult fetch(const char* key, sapper::CrackedLineSink& sink) override {
        ++fetchCount;
        lastKey = key != nullptr ? key : "";
        // A transport failure yields no lines, as a real connect/TLS failure would; a successful fetch
        // streams the scripted body.
        if (result == sapper::FetchResult::Ok) {
            for (const std::string& line : lines) sink.onLine(line);
        }
        return result;
    }
};

}  // namespace sapper_test
