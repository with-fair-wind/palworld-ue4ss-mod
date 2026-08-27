/**
 * @file fishing_boost_tests.cpp
 * @brief 钓鱼圣手纯值账本测试：记录/退役/停用/世界重置决策。
 */
#include <array>
#include <iostream>

#include <fishing_boost/fishing_boost_service.hpp>

namespace {
int failures = 0;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
}  // namespace

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_ledger_baseline_desired_and_phase() {
    fishing_boost::Ledger ledger;
    CHECK(!ledger.desired());
    CHECK(!ledger.has_records());
    CHECK(!ledger.safety_disabled());
    CHECK(ledger.phase() == fishing_boost::Phase::off);

    ledger.set_desired(true);
    CHECK(ledger.desired());
    ledger.set_desired(false);
    CHECK(!ledger.desired());
}

void test_record_and_clear_reset_retirement() {
    fishing_boost::Ledger ledger;
    ledger.retire_field(0);
    CHECK(ledger.is_field_retired(0));

    const std::array<float, fishing_boost::kFieldCount> values{1.0F, 2.0F, 3.0F, 4.0F};
    ledger.record_originals(values);
    CHECK(ledger.has_records());
    CHECK(!ledger.is_field_retired(0));  // 重新应用时退役位复位。

    ledger.retire_field(2);
    ledger.clear_records();
    CHECK(!ledger.has_records());
    CHECK(!ledger.is_field_retired(2));  // 清账同时复位退役位。
    CHECK(ledger.phase() == fishing_boost::Phase::off);
}

void test_retirement_bounds_and_isolation() {
    fishing_boost::Ledger ledger;
    ledger.retire_field(1);
    CHECK(ledger.is_field_retired(1));
    CHECK(!ledger.is_field_retired(0));  // 逐字段独立。
    CHECK(!ledger.is_field_retired(2));
    CHECK(!ledger.is_field_retired(3));
    CHECK(!ledger.is_field_retired(fishing_boost::kFieldCount));      // 越界安全。
    CHECK(!ledger.is_field_retired(fishing_boost::kFieldCount + 1));  // 越界安全。
    ledger.retire_field(fishing_boost::kFieldCount);                  // 越界退役无效化。
    CHECK(!ledger.is_field_retired(fishing_boost::kFieldCount));
}

void test_safety_disable_locks_until_world_reset() {
    fishing_boost::Ledger ledger;
    const std::array<float, fishing_boost::kFieldCount> values{5.0F, 6.0F, 7.0F, 8.0F};
    ledger.record_originals(values);
    ledger.disable_for_world();
    CHECK(ledger.safety_disabled());
    CHECK(ledger.phase() == fishing_boost::Phase::safetyDisabled);
    CHECK(ledger.has_records());  // 停用不清账：遗留记录待条件恢复。

    ledger.begin_world();
    CHECK(!ledger.safety_disabled());  // 世界重置解除停用。
    CHECK(!ledger.has_records());
    CHECK(ledger.phase() == fishing_boost::Phase::off);
}

void test_begin_world_clears_retirement() {
    fishing_boost::Ledger ledger;
    const std::array<float, fishing_boost::kFieldCount> values{1.0F, 1.0F, 1.0F, 1.0F};
    ledger.record_originals(values);
    ledger.retire_field(3);
    ledger.begin_world();
    CHECK(!ledger.is_field_retired(3));  // 新世界从零开始。
}

void test_active_phase_requires_records() {
    fishing_boost::Ledger ledger;
    ledger.set_desired(true);
    CHECK(ledger.phase() == fishing_boost::Phase::off);  // 未写入仍为 off。

    const std::array<float, fishing_boost::kFieldCount> values{0.5F, 0.6F, 0.7F, 0.8F};
    ledger.record_originals(values);
    CHECK(ledger.phase() == fishing_boost::Phase::active);
}

void test_originals_roundtrip() {
    fishing_boost::Ledger ledger;
    CHECK(!ledger.originals().has_value());
    const std::array<float, fishing_boost::kFieldCount> values{9.0F, 8.0F, 7.0F, 6.0F};
    ledger.record_originals(values);
    const auto restored = ledger.originals();
    CHECK(restored.has_value());
    CHECK((*restored)[0] == 9.0F);
    CHECK((*restored)[3] == 6.0F);
    ledger.clear_records();
    CHECK(!ledger.originals().has_value());
}

void test_safety_disable_overrides_active_phase() {
    fishing_boost::Ledger ledger;
    const std::array<float, fishing_boost::kFieldCount> values{1.0F, 2.0F, 3.0F, 4.0F};
    ledger.record_originals(values);
    CHECK(ledger.phase() == fishing_boost::Phase::active);
    ledger.disable_for_world();
    CHECK(ledger.phase() == fishing_boost::Phase::safetyDisabled);  // 停用优先于 active。
}

int main() {
    test_ledger_baseline_desired_and_phase();
    test_record_and_clear_reset_retirement();
    test_retirement_bounds_and_isolation();
    test_safety_disable_locks_until_world_reset();
    test_begin_world_clears_retirement();
    test_active_phase_requires_records();
    test_originals_roundtrip();
    test_safety_disable_overrides_active_phase();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "fishing_boost tests passed\n";
    return 0;
}
