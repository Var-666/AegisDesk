#include "support/test_builders.h"

#include "agent/health/health_checker.h"

#include <gtest/gtest.h>

namespace aegis::test {

TEST(HealthCheckerTest, RunningProcessIsHealthy) {
    ScopedTempDirectory directory("health-running");
    const agent::ServiceDefinition definition = ServiceDefinitionBuilder(directory.Path()).Build();
    agent::HealthChecker checker;

    const agent::HealthStatus result = checker.Check(definition, MakeRunningStatus(), 1000);

    EXPECT_EQ(result.state, agent::HealthState::kHealthy);
    EXPECT_EQ(result.consecutive_failures, 0U);
    EXPECT_EQ(result.checked_at_unix_ms, 1000);
}

TEST(HealthCheckerTest, StoppedProcessBecomesUnhealthyAtFailureThreshold) {
    ScopedTempDirectory directory("health-threshold");
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path()).WithFailureThreshold(3).Build();
    agent::HealthChecker checker;

    const agent::HealthStatus first = checker.Check(definition, MakeStoppedStatus(), 1000);
    const agent::HealthStatus second = checker.Check(definition, MakeStoppedStatus(), 2000);
    const agent::HealthStatus third = checker.Check(definition, MakeStoppedStatus(), 3000);

    EXPECT_EQ(first.state, agent::HealthState::kUnknown);
    EXPECT_EQ(second.state, agent::HealthState::kUnknown);
    EXPECT_EQ(third.state, agent::HealthState::kUnhealthy);
    EXPECT_EQ(third.consecutive_failures, 3U);
}

TEST(HealthCheckerTest, HealthyObservationResetsFailureCount) {
    ScopedTempDirectory directory("health-reset");
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path()).WithFailureThreshold(3).Build();
    agent::HealthChecker checker;

    EXPECT_EQ(checker.Check(definition, MakeStoppedStatus(), 1000).consecutive_failures, 1U);
    EXPECT_EQ(checker.Check(definition, MakeRunningStatus(), 2000).state, agent::HealthState::kHealthy);

    const agent::HealthStatus after_reset = checker.Check(definition, MakeStoppedStatus(), 3000);
    EXPECT_EQ(after_reset.state, agent::HealthState::kUnknown);
    EXPECT_EQ(after_reset.consecutive_failures, 1U);
}

TEST(HealthCheckerTest, DisabledCheckReturnsUnknownWithoutFailures) {
    ScopedTempDirectory directory("health-disabled");
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path()).WithHealthCheckEnabled(false).Build();
    agent::HealthChecker checker;

    const agent::HealthStatus result = checker.Check(definition, MakeStoppedStatus(), 1000);

    EXPECT_EQ(result.state, agent::HealthState::kUnknown);
    EXPECT_EQ(result.consecutive_failures, 0U);
    EXPECT_EQ(result.reason, "health check is disabled");
}

} // namespace aegis::test
