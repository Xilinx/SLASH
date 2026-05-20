/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef SIM_EXEC_LOG_HPP
#define SIM_EXEC_LOG_HPP

#include <cstdlib>
#include <string>

inline bool sim_exec_verbose_enabled() {
    static const bool enabled = []() {
        const char* raw = std::getenv("SIM_EXEC_VERBOSE");
        if (raw == nullptr) return false;
        std::string v(raw);
        for (char& c : v) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (v.empty() || v == "0" || v == "false" || v == "off" || v == "no") {
            return false;
        }
        return true;
    }();
    return enabled;
}

#define SIM_EXEC_LOG(stmt)          \
    do {                            \
        if (sim_exec_verbose_enabled()) { \
            stmt;                   \
        }                           \
    } while (0)

#endif  // SIM_EXEC_LOG_HPP
