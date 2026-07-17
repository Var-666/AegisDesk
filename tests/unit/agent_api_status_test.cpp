#include "support/test_builders.h"

#include "agent/api/agent_api.h"
#include "agent/health/health_monitor.h"
#include "agent/metrics/metrics_collector.h"
#include "agent/service/service_registry.h"

#include <boost/beast/http.hpp>
#include <gtest/gtest.h>

#include <fstream>
#include <string>

namespace aegis::test {
namespace {

void WriteServiceConfig(const std::filesystem::path& path) {
    std::ofstream output(path);
    ASSERT_TRUE(output.is_open());
    output << R"({
  "schema_version": 1,
  "services": [{
    "id": "api_service",
    "display_name": "API Service",
    "executable": "missing-service",
    "work_dir": ".",
    "args": [],
    "log_path": "api-service.log",
    "auto_start": false,
    "health_check": {
      "enabled": true,
      "type": "process",
      "interval_seconds": 5,
      "timeout_milliseconds": 1000,
      "failure_threshold": 3
    }
  }]
})";
}

agent::HttpRequest MakeGetRequest(const std::string& target) {
    agent::HttpRequest request;
    request.method(boost::beast::http::verb::get);
    request.target(target);
    request.version(11);
    return request;
}

} // namespace

TEST(AgentApiStatusTest, RetainsLegacyFieldsAndAddsLifecycleDiagnostics) {
    ScopedTempDirectory directory("agent-api-status");
    const std::filesystem::path config_path = directory.Path() / "services.json";
    WriteServiceConfig(config_path);

    agent::ServiceRegistry registry;
    std::string error;
    ASSERT_TRUE(registry.LoadFromFile(config_path, directory.Path(), error)) << error;

    agent::MetricsCollector metrics_collector(registry);
    agent::HealthMonitor health_monitor(registry, metrics_collector);
    agent::AgentApi api(registry, metrics_collector, health_monitor);

    const agent::HttpResponse response = api.Handle(MakeGetRequest("/api/v1/services/api_service/status"));
    ASSERT_EQ(response.result(), boost::beast::http::status::ok);

    const std::string& body = response.body();

    // Stable v1 fields remain available to existing clients.
    EXPECT_NE(body.find(R"("state":"Stopped")"), std::string::npos);
    EXPECT_NE(body.find(R"("desired_state":"stopped")"), std::string::npos);
    EXPECT_NE(body.find(R"("pid":-1)"), std::string::npos);
    EXPECT_NE(body.find(R"("uptime_seconds":0)"), std::string::npos);
    EXPECT_NE(body.find(R"("last_exit_code":null)"), std::string::npos);

    // Process Supervisor 2.0 appends lifecycle diagnostics without renaming v1 fields.
    EXPECT_NE(body.find(R"("process_group_id":-1)"), std::string::npos);
    EXPECT_NE(body.find(R"("last_exit_kind":"none")"), std::string::npos);
    EXPECT_NE(body.find(R"("last_exit_signal":null)"), std::string::npos);
    EXPECT_NE(body.find(R"("last_error":"")"), std::string::npos);
    EXPECT_NE(body.find(R"("last_transition_at_unix_ms":)"), std::string::npos);
}

TEST(AgentApiStatusTest, ExposesReliableStartFailureReason) {
    ScopedTempDirectory directory("agent-api-failure");
    const std::filesystem::path config_path = directory.Path() / "services.json";
    WriteServiceConfig(config_path);

    agent::ServiceRegistry registry;
    std::string error;
    ASSERT_TRUE(registry.LoadFromFile(config_path, directory.Path(), error)) << error;
    agent::ProcessSupervisor* supervisor = registry.Find("api_service");
    ASSERT_NE(supervisor, nullptr);
    ASSERT_FALSE(supervisor->Start(error));
    ASSERT_FALSE(error.empty());

    agent::MetricsCollector metrics_collector(registry);
    agent::HealthMonitor health_monitor(registry, metrics_collector);
    agent::AgentApi api(registry, metrics_collector, health_monitor);

    const agent::HttpResponse response = api.Handle(MakeGetRequest("/api/v1/services/api_service/status"));
    ASSERT_EQ(response.result(), boost::beast::http::status::ok);
    EXPECT_NE(response.body().find(R"("state":"Failed")"), std::string::npos);
    EXPECT_NE(response.body().find(R"("desired_state":"running")"), std::string::npos);
    EXPECT_NE(response.body().find(R"("last_error":")"), std::string::npos);
    EXPECT_NE(response.body().find("does not exist"), std::string::npos);
}

} // namespace aegis::test
