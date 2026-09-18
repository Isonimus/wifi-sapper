/**
 * @file native/mock/Preferences.h
 * @brief In-memory fake of the Arduino `Preferences` (NVS) API for the native test lane (ADR-0008).
 *
 * Device builds resolve <Preferences.h> to the real ESP-IDF header; this file is on the include
 * path of env:native ONLY (`-I native/mock`), so `net/provisioning_store.cpp` compiles unchanged on
 * the host. It models just the surface that file uses — begin/getString/putString/clear/end — plus
 * a fault-injection knob so a partial NVS write (a real hardware failure mode) can be reproduced on
 * the host, which the hardware lane cannot induce on demand. It is not a general NVS emulator; it
 * grows a call only when the store gains a real one (ADR-0008, rule of three).
 */
#pragma once

#ifndef UNIT_TEST
#error "native/mock/Preferences.h must only be compiled in the native (UNIT_TEST) lane (ADR-0008)"
#endif

#include <cstddef>
#include <cstring>
#include <map>
#include <string>

namespace sapper_test {

/// The whole fake NVS: namespace -> (key -> value). Static so it persists across `Preferences`
/// instances within a test, the way flash persists across begin/end cycles. `inline` gives it one
/// definition across every native translation unit.
inline std::map<std::string, std::map<std::string, std::string>>& nvsStore() {
    static std::map<std::string, std::map<std::string, std::string>> store;
    return store;
}

/// Namespaces that have ever been opened read/write. A read-only `begin` on one never created
/// returns false, exactly as real NVS reports "not provisioned" on a first boot.
inline std::map<std::string, bool>& nvsCreated() {
    static std::map<std::string, bool> created;
    return created;
}

/// Fault injection: when non-empty, `putString` for this key reports a short count and stores
/// nothing, reproducing a mid-triad NVS write failure. Empty means no injected fault.
inline std::string& failPutKey() {
    static std::string key;
    return key;
}

/// Reset the fake between tests: empty flash, no created namespaces, no injected fault.
inline void resetNvs() {
    nvsStore().clear();
    nvsCreated().clear();
    failPutKey().clear();
}

}  // namespace sapper_test

/// Drop-in for the subset of Arduino `Preferences` that the provisioning store calls.
class Preferences {
public:
    bool begin(const char* name, bool readOnly = false) {
        m_name = name;
        m_open = true;
        if (readOnly) {
            return sapper_test::nvsCreated().count(m_name) != 0;
        }
        sapper_test::nvsCreated()[m_name] = true;  // a read/write begin creates the namespace.
        return true;
    }

    size_t putString(const char* key, const char* value) {
        if (!m_open) return 0;
        if (!sapper_test::failPutKey().empty() && sapper_test::failPutKey() == key) {
            return 0;  // injected fault: short write, nothing stored.
        }
        sapper_test::nvsStore()[m_name][key] = value;
        return std::strlen(value);
    }

    size_t getString(const char* key, char* out, size_t maxLen) {
        if (out == nullptr || maxLen == 0) return 0;
        out[0] = '\0';
        if (!m_open) return 0;
        const auto ns = sapper_test::nvsStore().find(m_name);
        if (ns == sapper_test::nvsStore().end()) return 0;
        const auto entry = ns->second.find(key);
        if (entry == ns->second.end()) return 0;
        const std::string& value = entry->second;
        const size_t copied = value.size() < maxLen - 1 ? value.size() : maxLen - 1;
        std::memcpy(out, value.data(), copied);
        out[copied] = '\0';
        return copied;
    }

    bool clear() {
        if (!m_open) return false;
        sapper_test::nvsStore()[m_name].clear();
        return true;
    }

    void end() { m_open = false; }

private:
    std::string m_name;
    bool m_open = false;
};
