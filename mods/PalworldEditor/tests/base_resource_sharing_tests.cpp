#include <array>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <vector>

#include <base_resource_sharing/hook_manifest.hpp>
#include <base_resource_sharing/resource_pool.hpp>
#include <base_resource_sharing/resource_session.hpp>
#include <base_resource_sharing/settings.hpp>

namespace {
auto failures = 0;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
}  // namespace

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_settings_default_off_and_round_trip() {
    using namespace base_resource_sharing;

    const auto missing = parse_settings("");
    CHECK(!missing.settings.enabled);
    CHECK(!missing.error.empty());

    const auto enabled = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=true\n");
    CHECK(enabled.settings.enabled);
    CHECK(enabled.error.empty());
    CHECK(serialize_settings(enabled.settings) == "[BaseResourceSharing]\nEnabled=true\n");

    const auto invalid = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=maybe\n");
    CHECK(!invalid.settings.enabled);
    CHECK(!invalid.error.empty());
}

void test_settings_file_round_trip() {
    using namespace base_resource_sharing;

    const auto root =
        std::filesystem::temp_directory_path() / "PalworldEditorBaseResourceSharingTests";
    const auto path = root / "config.ini";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const auto missing = load_settings(path);
    CHECK(!missing.settings.enabled);
    CHECK(!missing.error.empty());
    CHECK(!std::filesystem::exists(path));

    CHECK(save_settings(path, Settings{.enabled = true}).empty());
    const auto loaded = load_settings(path);
    CHECK(loaded.settings.enabled);
    CHECK(loaded.error.empty());

    std::filesystem::remove_all(root, ignored);
}

void test_resource_pool_filters_deduplicates_and_orders() {
    using namespace base_resource_sharing;

    const GuidKey guild{{1, 0, 0, 0}};
    const GuidKey otherGuild{{2, 0, 0, 0}};
    const GuidKey baseA{{10, 0, 0, 0}};
    const GuidKey baseB{{20, 0, 0, 0}};
    const std::vector<ContainerDescriptor> containers{
        {.baseId = baseB,
         .groupId = guild,
         .containerId = {{202, 0, 0, 0}},
         .kind = ContainerKind::normal},
        {.baseId = baseA,
         .groupId = guild,
         .containerId = {{101, 0, 0, 0}},
         .kind = ContainerKind::normal,
         .currentBase = true},
        {.baseId = baseA,
         .groupId = guild,
         .containerId = {{101, 0, 0, 0}},
         .kind = ContainerKind::normal,
         .currentBase = true},
        {.baseId = baseA,
         .groupId = guild,
         .containerId = {{102, 0, 0, 0}},
         .kind = ContainerKind::food,
         .currentBase = true},
        {.baseId = baseB,
         .groupId = otherGuild,
         .containerId = {{203, 0, 0, 0}},
         .kind = ContainerKind::normal},
    };

    const auto plan = make_resource_union_plan(containers, guild);
    CHECK(plan.error.empty());
    CHECK(plan.baseCount == 2);
    CHECK(plan.ordered.size() == 2);
    const GuidKey expectedFirst{{101, 0, 0, 0}};
    const GuidKey expectedSecond{{202, 0, 0, 0}};
    CHECK(plan.ordered[0].containerId == expectedFirst);
    CHECK(plan.ordered[1].containerId == expectedSecond);

    CHECK(!make_resource_union_plan(containers, {}).error.empty());
    CHECK(!make_resource_union_plan({}, guild).error.empty());
}

void test_runtime_state_fails_closed_across_worlds() {
    using namespace base_resource_sharing;

    RuntimeState state;
    state.set_preference(true);
    state.finish_world_transition(7);
    state.set_capability(ResourceOperation::crafting,
                         CapabilityState{.previewReady = true, .consumeReady = true});

    CHECK(state.can_extend(ResourceOperation::crafting, 7));
    CHECK(!state.can_extend(ResourceOperation::building, 7));

    state.begin_world_transition(8);
    CHECK(!state.can_extend(ResourceOperation::crafting, 7));
    CHECK(!state.can_extend(ResourceOperation::crafting, 8));

    state.finish_world_transition(8);
    CHECK(!state.can_extend(ResourceOperation::crafting, 8));
}

void test_hook_capabilities_require_preview_and_consume_paths() {
    using namespace base_resource_sharing;

    auto resolved = all_hook_resolutions(false);
    for (const auto& hook : palworld_1_0_1_hook_manifest()) {
        if (hook.operation == ResourceOperation::crafting &&
            hook.requirement == HookRequirement::required) {
            mark_resolved(resolved, hook.path);
        }
    }
    auto capabilities = evaluate_capabilities(resolved);
    CHECK(capabilities[operation_index(ResourceOperation::crafting)].available());
    CHECK(!capabilities[operation_index(ResourceOperation::building)].available());
    CHECK(!capabilities[operation_index(ResourceOperation::repair)].available());

    for (const auto& hook : palworld_1_0_1_hook_manifest()) {
        if (hook.operation == ResourceOperation::building &&
            hook.requirement == HookRequirement::required) {
            mark_resolved(resolved, hook.path);
        }
    }
    capabilities = evaluate_capabilities(resolved);
    CHECK(capabilities[operation_index(ResourceOperation::building)].available());
    CHECK(capabilities[operation_index(ResourceOperation::repair)].error ==
          "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。");
}

void test_union_plan_appends_only_missing_container_ids() {
    using namespace base_resource_sharing;

    const GuidKey a{{1, 0, 0, 0}};
    const GuidKey b{{2, 0, 0, 0}};
    const GuidKey c{{3, 0, 0, 0}};
    const std::array original{a};
    const std::array partialPlan{a, c};
    const std::array globalPlan{a, b, c};
    CHECK(missing_union_tail(original, globalPlan) == std::vector<GuidKey>({b, c}));
    CHECK(missing_union_tail(partialPlan, globalPlan) == std::vector<GuidKey>({b}));
}

void test_recorded_injection_removal_preserves_runtime_native_changes() {
    using namespace base_resource_sharing;

    const GuidKey a{{1, 0, 0, 0}};
    const GuidKey b{{2, 0, 0, 0}};
    const GuidKey c{{3, 0, 0, 0}};
    const GuidKey d{{4, 0, 0, 0}};
    const std::array original{a};
    const std::array current{a, b, c, d, b};
    const std::array injected{b, c};

    const auto plan = remove_recorded_injections(current, original, injected);
    CHECK(plan.complete);
    CHECK(plan.kept == std::vector<GuidKey>({a, d, b}));

    const std::array missing{a, b};
    CHECK(!remove_recorded_injections(missing, original, injected).complete);
}

void test_status_text_reports_partial_support() {
    using namespace base_resource_sharing;

    const BaseResourceSharingStatus status{
        .enabled = true,
        .baseCount = 3,
        .containerCount = 12,
        .craftingAvailable = true,
        .buildingAvailable = true,
        .repairAvailable = false,
        .repairError = "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。",
    };
    const auto text = format_status(status);
    CHECK(text.find("3 个据点") != std::string::npos);
    CHECK(text.find("12 个资源容器") != std::string::npos);
    CHECK(text.find("制作：可用") != std::string::npos);
    CHECK(text.find("建造：可用") != std::string::npos);
    CHECK(text.find("修理：不可用") != std::string::npos);

    const BaseResourceSharingStatus detecting{
        .enabled = true,
        .detectingCapabilities = true,
        .craftingError = "缺少必需接口：CraftPreview",
        .buildingError = "缺少必需接口：BuildConsume",
    };
    const auto detectingText = format_status(detecting);
    CHECK(detectingText.find("CraftPreview") != std::string::npos);
    CHECK(detectingText.find("BuildConsume") != std::string::npos);
}

void test_disabled_resource_sharing_has_no_runtime_work() {
    using namespace base_resource_sharing;

    CHECK(!resource_hooks_required(false, true));
    CHECK(!resource_hooks_required(true, false));
    CHECK(resource_hooks_required(true, true));

    SnapshotDirtyFlag dirty;
    CHECK(dirty.consume());
    CHECK(!dirty.consume());
    dirty.mark();
    CHECK(dirty.consume());
}

void test_hook_manifest_separates_required_sessions_from_optional_events() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    CHECK(std::ranges::count(hooks, HookAction::structureChanged, &HookSpec::action) == 4);
    CHECK(std::ranges::count(hooks, HookAction::buildingModeChanged, &HookSpec::action) == 1);
    CHECK(std::ranges::count(hooks, HookAction::buildingTouch, &HookSpec::action) == 1);
    CHECK(std::ranges::count(hooks, HookAction::craftingAcquire, &HookSpec::action) == 1);
    CHECK(std::ranges::count(hooks, HookAction::craftingTouch, &HookSpec::action) == 3);

    auto resolved = all_hook_resolutions(false);
    for (const auto& hook : hooks) {
        if (hook.requirement == HookRequirement::optional) {
            mark_resolved(resolved, hook.path);
        }
    }
    auto capabilities = evaluate_capabilities(resolved);
    CHECK(!capabilities[operation_index(ResourceOperation::crafting)].available());
    CHECK(!capabilities[operation_index(ResourceOperation::building)].available());

    for (const auto& hook : hooks) {
        if (hook.operation == ResourceOperation::building &&
            hook.requirement == HookRequirement::required) {
            mark_resolved(resolved, hook.path);
        }
    }
    capabilities = evaluate_capabilities(resolved);
    CHECK(!capabilities[operation_index(ResourceOperation::crafting)].available());
    CHECK(capabilities[operation_index(ResourceOperation::building)].available());
}

void test_resource_toggle_transition_distinguishes_disable_and_accessible_reenable() {
    using namespace base_resource_sharing;

    auto transition = decide_resource_toggle(false, false, true);
    CHECK(!transition.disableRuntime);
    CHECK(!transition.beginAccessibleWorld);

    transition = decide_resource_toggle(true, true, true);
    CHECK(!transition.disableRuntime);
    CHECK(!transition.beginAccessibleWorld);

    transition = decide_resource_toggle(true, false, true);
    CHECK(transition.disableRuntime);
    CHECK(!transition.beginAccessibleWorld);

    transition = decide_resource_toggle(false, true, false);
    CHECK(!transition.disableRuntime);
    CHECK(!transition.beginAccessibleWorld);

    transition = decide_resource_toggle(false, true, true);
    CHECK(!transition.disableRuntime);
    CHECK(transition.beginAccessibleWorld);
}

void test_reconcile_scheduler_coalesces_events_and_uses_bounded_intervals() {
    using namespace base_resource_sharing;

    ReconcileScheduler scheduler;
    scheduler.begin_world(7);
    CHECK(scheduler.advance(0.0F, 7));
    scheduler.complete(true, 7);
    CHECK(!scheduler.advance(7.999F, 7));
    CHECK(scheduler.advance(0.001F, 7));
    scheduler.complete(true, 7);

    scheduler.request_immediate(7);
    scheduler.request_immediate(7);
    CHECK(scheduler.advance(0.0F, 7));
    CHECK(!scheduler.advance(0.0F, 7));
    scheduler.complete(false, 7);
    CHECK(!scheduler.advance(0.999F, 7));
    CHECK(scheduler.advance(0.001F, 7));
    scheduler.complete(true, 7);

    CHECK(!scheduler.advance(8.0F, 8));

    scheduler.reset();
    CHECK(!scheduler.advance(0.0F, 7));
    scheduler.begin_world(7);
    CHECK(scheduler.advance(0.0F, 7));
    scheduler.complete(true, 7);
    CHECK(!scheduler.advance(0.0F, 7));
}

void test_union_leases_overlap_and_crafting_expires_after_idle() {
    using namespace base_resource_sharing;

    ResourceUnionLeaseState leases;
    leases.begin_world(11);
    CHECK(leases.acquire_building(11));
    CHECK(leases.touch_crafting(11));
    CHECK(leases.desired(11));
    CHECK(!leases.advance(1.5F, 11));
    CHECK(leases.desired(11));
    CHECK(!leases.release_building(12));
    CHECK(leases.release_building(11));
    CHECK(!leases.desired(11));

    CHECK(leases.touch_crafting(11));
    CHECK(!leases.advance(1.499F, 11));
    CHECK(leases.advance(0.001F, 11));
    CHECK(!leases.desired(11));
    CHECK(!leases.touch_crafting(12));
}

auto main() -> int {
    test_settings_default_off_and_round_trip();
    test_settings_file_round_trip();
    test_resource_pool_filters_deduplicates_and_orders();
    test_runtime_state_fails_closed_across_worlds();
    test_hook_capabilities_require_preview_and_consume_paths();
    test_union_plan_appends_only_missing_container_ids();
    test_recorded_injection_removal_preserves_runtime_native_changes();
    test_status_text_reports_partial_support();
    test_disabled_resource_sharing_has_no_runtime_work();
    test_hook_manifest_separates_required_sessions_from_optional_events();
    test_resource_toggle_transition_distinguishes_disable_and_accessible_reenable();
    test_reconcile_scheduler_coalesces_events_and_uses_bounded_intervals();
    test_union_leases_overlap_and_crafting_expires_after_idle();
    return failures == 0 ? 0 : 1;
}
