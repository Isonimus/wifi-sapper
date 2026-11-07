/**
 * @file web_arg_reader.h
 * @brief Device adapter wiring a live `WebServer`'s POSTed args to the pure `FormArgReader` seam.
 *
 * Device-only (wraps Arduino `WebServer`); excluded from the native lane, where the parser is driven by
 * a fake reader instead. The thin glue that lets `parseSetupFormSubmission` stay pure and host-tested
 * (ADR-0114): both the captive portal and the Maintenance `/config` editor construct one of these per
 * save and hand it to the parser, so the submitted-field list lives only in the pure parser, never
 * duplicated across the two serving surfaces (revises ADR-0043 decision 6).
 */
#pragma once

#ifndef UNIT_TEST

#include <WebServer.h>

#include "net/provisioning_form.h"  // FormArgReader

namespace sapper {

/**
 * @brief Exposes a `WebServer`'s request args to the pure parser.
 *
 * `value()` copies the arg into a reused scratch `String`, so the returned pointer is valid only until
 * the next `value()` call — exactly the parser's copy-then-advance access pattern (FormArgReader's
 * contract). One reader is stack-constructed per save handler; the serve is single-threaded, so the
 * reuse is safe. `WebServer::arg` returns an empty `String` for an absent field, which satisfies the
 * seam's "empty, never nullptr" contract for a missing (e.g. unchecked checkbox) field.
 */
class WebServerArgReader final : public FormArgReader {
public:
    explicit WebServerArgReader(WebServer& http) : m_http(http) {}

    const char* value(const char* name) const override {
        m_scratch = m_http.arg(name);
        return m_scratch.c_str();
    }

private:
    WebServer& m_http;
    mutable String m_scratch;
};

}  // namespace sapper

#endif  // UNIT_TEST
