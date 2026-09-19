/**
 * @file fake_window_notifier.h
 * @brief A test WindowNotifier that counts the in-window flushes the supervisor drives (ADR-0023).
 *
 * The supervisor calls flushInWindow() once per drain cycle while the station is up; counting the calls
 * is how the supervisor test proves a transmitting surface is offered every live window and no offline
 * one.
 */
#pragma once

#include "net/window_notifier.h"

namespace sapper_test {

class FakeWindowNotifier : public sapper::WindowNotifier {
public:
    int flushCalls = 0;

    void flushInWindow() override { ++flushCalls; }
};

}  // namespace sapper_test
