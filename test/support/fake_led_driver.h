/**
 * @file fake_led_driver.h
 * @brief A test LedDriver that records the status transitions it is shown (ADR-0021).
 *
 * The surface calls show() only when the displayed status changes, so `shown` is exactly the
 * transition sequence — which is the observable behaviour the LED policy tests assert against.
 */
#pragma once

#include <vector>

#include "surface/led_driver.h"

namespace sapper_test {

class FakeLedDriver : public sapper::LedDriver {
public:
    std::vector<sapper::LedStatus> shown;

    void show(sapper::LedStatus status) override { shown.push_back(status); }

    sapper::LedStatus last() const {
        return shown.empty() ? sapper::LedStatus::Off : shown.back();
    }
};

}  // namespace sapper_test
