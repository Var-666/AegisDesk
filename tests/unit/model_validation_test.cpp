#include "support/test_builders.h"

#include "agent/service/service_id.h"

#include <gtest/gtest.h>

namespace aegis::test {

TEST(ServiceIdTest, AcceptsSupportedCharacters) {
    EXPECT_TRUE(agent::IsValidServiceId("service"));
    EXPECT_TRUE(agent::IsValidServiceId("Demo_Service-42"));
}

TEST(ServiceIdTest, RejectsEmptyWhitespaceAndPunctuation) {
    EXPECT_FALSE(agent::IsValidServiceId(""));
    EXPECT_FALSE(agent::IsValidServiceId("demo service"));
    EXPECT_FALSE(agent::IsValidServiceId("demo.service"));
    EXPECT_FALSE(agent::IsValidServiceId("service/child"));
}

TEST(ServiceDefinitionTest, BuilderProducesAValidDefinition) {
    ScopedTempDirectory directory("definition");

    const agent::ServiceDefinition definition = ServiceDefinitionBuilder(directory.Path()).Build();

    EXPECT_TRUE(definition.IsStructurallyValid());
    EXPECT_EQ(definition.executable, ServiceDefinitionBuilder::FaultProcessPath());
    EXPECT_EQ(definition.work_dir, directory.Path());
}

TEST(AlertRuleTest, CpuRuleBuilderProducesAValidRule) {
    const agent::AlertRule rule = MakeCpuAlertRule();

    EXPECT_TRUE(rule.IsStructurallyValid());
    EXPECT_EQ(rule.metric, agent::AlertMetric::kCpuPercent);
    ASSERT_TRUE(rule.numeric_threshold.has_value());
    EXPECT_DOUBLE_EQ(*rule.numeric_threshold, 80.0);
}

TEST(ServiceMetricsTest, MetricsBuilderProducesAValidSample) {
    const agent::ServiceMetrics metrics = MakeAvailableMetrics();

    EXPECT_TRUE(metrics.IsStructurallyValid());
    EXPECT_TRUE(metrics.HasAnyMetricValue());
}

} // namespace aegis::test
