#include "agent/platform_metrics_reader.h"

#include <memory>
#include <string>

namespace aegis::agent {
namespace {

class UnsupportedMetricsReader final : public PlatformMetricsReader {
public:
    std::optional<ProcessRawStats> ReadProcessRawStats(pid_t, std::string& error) override {
        error = "metrics collection is not supported on this platform";

        return std::nullopt;
    }

    std::optional<SystemCapacitySnapshot> ReadSystemCapacity(std::string& error) override {
        error = "metrics collection is not supported on this platform";

        return std::nullopt;
    }
};

} // namespace

std::unique_ptr<PlatformMetricsReader> CreatePlatformMetricsReader() {
    return std::make_unique<UnsupportedMetricsReader>();
}

} // namespace aegis::agent