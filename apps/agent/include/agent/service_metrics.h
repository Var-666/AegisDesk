#pragma once

#include "service_definition.h"

#include <optional>
#include <string>

namespace aegis::agent {
using UnixTimeMilliseconds = std::int64_t;

struct ServiceMetrics {
    std::string service_id;
    bool available{false};
    std::int64_t pid{-1};
    UnixTimeMilliseconds collected_at_unix_ms{0};

    std::optional<double> cpu_percent;
    std::optional<std::uint64_t> rss_bytes;
    std::optional<std::uint64_t> thread_count;
    std::optional<std::uint64_t> fd_count;

    [[nodiscard]] bool HasAnyMetricValue() const noexcept {
        return cpu_percent.has_value() || rss_bytes.has_value() || thread_count.has_value() || fd_count.has_value();
    }

    [[nodiscard]] bool IsStructurallyValid() const noexcept {
        if (!IsValidServiceId(service_id) || pid < -1 || collected_at_unix_ms < 0) {
            return false;
        }

        if (cpu_percent.has_value()) {
            if (!std::isfinite(*cpu_percent) || *cpu_percent < 0.0 || *cpu_percent > 100.0) {
                return false;
            }
        }

        if (!available) {
            return !HasAnyMetricValue();
        }

        if (pid <= 0) {
            return false;
        }

        return rss_bytes.has_value() && thread_count.has_value() && fd_count.has_value();
    }
};
} // namespace aegis::agent