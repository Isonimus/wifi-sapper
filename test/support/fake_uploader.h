/**
 * @file fake_uploader.h
 * @brief Scripted Uploader double for the host lane (ADR-0017 decision #2; slice-0018).
 *
 * Returns a pre-scripted sequence of UploadResults so a test drives the supervisor's retry/backoff
 * and delete-on-success logic without a network. It records every (bytes, key) it was handed so a
 * test can assert each capture was uploaded once and with the right key.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/uploader.h"

namespace sapper_test {

class FakeUploader : public sapper::Uploader {
public:
    struct Call {
        std::vector<uint8_t> pcap;
        std::string key;
    };

    sapper::UploadResult upload(const uint8_t* pcap, size_t len, const char* key) override {
        calls.push_back({std::vector<uint8_t>(pcap, pcap + len), std::string(key)});
        if (!scripted.empty()) {
            const sapper::UploadResult result = scripted.front();
            if (scripted.size() > 1) scripted.erase(scripted.begin());  // last result repeats.
            return result;
        }
        return defaultResult;
    }

    // --- Test knobs and observation. ---
    /// Results returned in order; the final one repeats once exhausted. Empty → defaultResult always.
    std::vector<sapper::UploadResult> scripted;
    sapper::UploadResult defaultResult = sapper::UploadResult::Accepted;
    std::vector<Call> calls;
};

}  // namespace sapper_test
