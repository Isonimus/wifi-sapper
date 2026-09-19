/**
 * @file fake_webhook_transport.h
 * @brief A test WebhookTransport that records each posted request and scripts success/failure (ADR-0023).
 *
 * The notifier renders into borrowed scratch buffers valid only for the post() call, so this fake COPIES
 * every field (and remembers which optional headers were present) at post() time — that copy is the
 * observable behaviour the rendering/flush tests assert against. `succeed` scripts the result so a test
 * can prove send-and-keep retry (fail one flush, then accept the next).
 */
#pragma once

#include <string>
#include <vector>

#include "surface/webhook_notifier.h"

namespace sapper_test {

class FakeWebhookTransport : public sapper::WebhookTransport {
public:
    struct RecordedPost {
        std::string url;
        std::string body;
        std::string contentType;  ///< empty iff the request had no Content-Type (ntfy plain).
        std::string title;        ///< empty iff the request had no Title header (Discord).
        bool hadContentType = false;
        bool hadTitle = false;
    };

    std::vector<RecordedPost> posts;
    bool succeed = true;  ///< the result post() returns; flip between flushes to script retry.

    bool post(const sapper::WebhookRequest& request) override {
        RecordedPost rec;
        rec.url = request.url ? request.url : "";
        rec.body = request.body ? request.body : "";
        rec.hadContentType = request.contentType != nullptr;
        rec.hadTitle = request.title != nullptr;
        if (rec.hadContentType) rec.contentType = request.contentType;
        if (rec.hadTitle) rec.title = request.title;
        posts.push_back(rec);
        return succeed;
    }
};

}  // namespace sapper_test
