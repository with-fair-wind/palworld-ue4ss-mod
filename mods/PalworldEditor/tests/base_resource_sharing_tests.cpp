#include <array>
#include <iostream>
#include <vector>

#include <base_resource_sharing/hook_manifest.hpp>
#include <base_resource_sharing/resource_pool.hpp>
#include <base_resource_sharing/resource_session.hpp>
#include <grappling_hook/cooldown_gateway.hpp>
#include <grappling_hook/cooldown_service.hpp>

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

void test_hook_manifest_acquires_before_first_build_and_craft_eligibility() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    const auto buildOpen = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalUIBuildModel:OnOpenMenu"}, &HookSpec::path);
    CHECK(buildOpen != hooks.end());
    CHECK(event_for_phase(*buildOpen, HookPhase::pre) == HookEvent::acquire);
    CHECK(event_for_phase(*buildOpen, HookPhase::post) == HookEvent::none);
    CHECK(buildOpen->requirement == HookRequirement::required);

    const auto craftInitialize = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalUIConvertItemModel:Initialize"}, &HookSpec::path);
    CHECK(craftInitialize != hooks.end());
    CHECK(event_for_phase(*craftInitialize, HookPhase::pre) == HookEvent::acquire);
    CHECK(event_for_phase(*craftInitialize, HookPhase::post) == HookEvent::none);
    CHECK(craftInitialize->requirement == HookRequirement::required);
}

void test_hook_manifest_keeps_crafting_alive_through_submission() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    const auto startProduction = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalUIConvertItemModel:StartProduction"},
        &HookSpec::path);
    CHECK(startProduction != hooks.end());
    CHECK(event_for_phase(*startProduction, HookPhase::pre) == HookEvent::touch);
    CHECK(event_for_phase(*startProduction, HookPhase::post) == HookEvent::touch);

    for (const auto& hook : hooks) {
        if (event_for_phase(hook, HookPhase::pre) == HookEvent::touch) {
            CHECK(event_for_phase(hook, HookPhase::pre) != HookEvent::acquire);
        }
        if (event_for_phase(hook, HookPhase::post) == HookEvent::structureChanged) {
            CHECK(event_for_phase(hook, HookPhase::pre) == HookEvent::none);
        }
    }
}

void test_missing_early_build_acquire_disables_only_building() {
    using namespace base_resource_sharing;

    auto resolved = all_hook_resolutions(true);
    for (auto& resolution : resolved) {
        if (resolution.spec.path == "/Script/Pal.PalUIBuildModel:OnOpenMenu") {
            resolution.resolved = false;
        }
    }
    const auto capabilities = evaluate_capabilities(resolved);
    CHECK(!capabilities[operation_index(ResourceOperation::building)].available());
    CHECK(capabilities[operation_index(ResourceOperation::crafting)].available());
    CHECK(!capabilities[operation_index(ResourceOperation::repair)].available());
}

void test_hook_manifest_tracks_current_base_context() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    const auto enter = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalBuilderComponent:OnEnterBaseCamp"},
        &HookSpec::path);
    const auto exit = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalBuilderComponent:OnExitBaseCamp"}, &HookSpec::path);
    CHECK(enter != hooks.end());
    CHECK(exit != hooks.end());
    if (enter != hooks.end()) {
        CHECK(event_for_phase(*enter, HookPhase::pre) == HookEvent::enterBase);
    }
    if (exit != hooks.end()) {
        CHECK(event_for_phase(*exit, HookPhase::pre) == HookEvent::exitBase);
    }
}

void test_missing_current_base_hook_disables_only_building() {
    using namespace base_resource_sharing;

    auto resolved = all_hook_resolutions(true);
    for (auto& resolution : resolved) {
        if (resolution.spec.path == "/Script/Pal.PalBuilderComponent:OnEnterBaseCamp") {
            resolution.resolved = false;
        }
    }
    const auto capabilities = evaluate_capabilities(resolved);
    CHECK(!capabilities[operation_index(ResourceOperation::building)].available());
    CHECK(capabilities[operation_index(ResourceOperation::crafting)].available());
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
    CHECK(scheduler.advance(0.0F, 7, true));
    scheduler.complete(true, 7);
    CHECK(!scheduler.advance(20.0F, 7, false));
    CHECK(!scheduler.advance(7.999F, 7, true));
    CHECK(scheduler.advance(0.001F, 7, true));
    scheduler.complete(true, 7);

    scheduler.request_immediate(7);
    scheduler.request_immediate(7);
    CHECK(!scheduler.advance(10.0F, 7, false));
    CHECK(scheduler.advance(0.0F, 7, true));
    CHECK(!scheduler.advance(0.0F, 7, true));
    scheduler.complete(false, 7);
    CHECK(!scheduler.advance(0.999F, 7, true));
    CHECK(scheduler.advance(0.001F, 7, true));
    scheduler.complete(true, 7);

    CHECK(!scheduler.advance(8.0F, 8, true));

    scheduler.reset();
    CHECK(!scheduler.advance(0.0F, 7, true));
    scheduler.begin_world(7);
    CHECK(scheduler.advance(0.0F, 7, true));
    scheduler.complete(true, 7);
    CHECK(!scheduler.advance(0.0F, 7, true));
}

void test_foreground_session_preempts_instead_of_combining_operations() {
    using namespace base_resource_sharing;

    ForegroundMaterialSession sessions;
    sessions.begin_world(7);
    const auto crafting = sessions.acquire(ResourceOperation::crafting, 7);
    CHECK(crafting.kind == ForegroundTransitionKind::acquired);
    CHECK(sessions.active(7) == ResourceOperation::crafting);

    const auto building = sessions.acquire(ResourceOperation::building, 7);
    CHECK(building.kind == ForegroundTransitionKind::preempted);
    CHECK(building.previous == ResourceOperation::crafting);
    CHECK(building.current == ResourceOperation::building);
    CHECK(sessions.active(7) == ResourceOperation::building);
}

void test_foreground_session_ignores_stale_touch_and_release() {
    using namespace base_resource_sharing;

    ForegroundMaterialSession sessions;
    sessions.begin_world(7);
    static_cast<void>(sessions.acquire(ResourceOperation::building, 7));
    CHECK(!sessions.touch(ResourceOperation::crafting, 7));
    CHECK(sessions.release(ResourceOperation::crafting, 7).kind == ForegroundTransitionKind::none);
    CHECK(!sessions.active(8).has_value());
}

void test_foreground_crafting_session_expires_only_after_idle_lease() {
    using namespace base_resource_sharing;

    ForegroundMaterialSession sessions;
    sessions.begin_world(7);
    static_cast<void>(sessions.acquire(ResourceOperation::crafting, 7));
    CHECK(sessions.advance(1.49F, 7).kind == ForegroundTransitionKind::none);
    CHECK(sessions.touch(ResourceOperation::crafting, 7));
    CHECK(sessions.advance(1.49F, 7).kind == ForegroundTransitionKind::none);
    const auto expired = sessions.advance(0.01F, 7);
    CHECK(expired.kind == ForegroundTransitionKind::released);
    CHECK(expired.previous == ResourceOperation::crafting);
    CHECK(!sessions.active(7).has_value());
}

void test_current_base_state_never_leaks_across_worlds() {
    using namespace base_resource_sharing;

    CurrentBaseState state;
    const GuidKey baseA{{10, 0, 0, 0}};
    const GuidKey baseB{{20, 0, 0, 0}};
    state.begin_world(7);
    CHECK(state.enter(baseA, 7));
    CHECK(state.current(7) == baseA);
    CHECK(!state.exit(baseB, 7));
    CHECK(state.current(7) == baseA);
    CHECK(state.exit(baseA, 7));
    CHECK(!state.current(7).has_value());
    state.begin_world(8);
    CHECK(!state.current(8).has_value());
}

void test_resource_exposure_uses_exactly_one_consumer_surface() {
    using namespace base_resource_sharing;

    const GuidKey base{{10, 0, 0, 0}};
    const auto crafting = make_exposure_plan(ResourceOperation::crafting);
    CHECK(crafting.surface == ResourceConsumerSurface::playerHelper);
    CHECK(!crafting.targetBaseId.has_value());

    const auto building = make_exposure_plan(ResourceOperation::building, base);
    CHECK(building.surface == ResourceConsumerSurface::currentBaseModule);
    CHECK(building.targetBaseId == base);

    CHECK(make_exposure_plan(ResourceOperation::building).surface == ResourceConsumerSurface::none);
    CHECK(make_exposure_plan(ResourceOperation::repair).surface == ResourceConsumerSurface::none);
}

void test_applied_sequence_rejects_duplicate_remote_container() {
    using namespace base_resource_sharing;

    const GuidKey local{{1, 0, 0, 0}};
    const GuidKey remote{{2, 0, 0, 0}};
    const std::array original{local};
    const std::array injected{remote};
    const std::array valid{local, remote};
    const std::array doubled{local, remote, remote};

    CHECK(static_cast<bool>(validate_applied_sequence(original, injected, valid)));
    CHECK(!validate_applied_sequence(original, injected, doubled));
}

void test_applied_sequence_rejects_injection_of_existing_id() {
    using namespace base_resource_sharing;

    const GuidKey local{{1, 0, 0, 0}};
    const std::array original{local};
    const std::array injected{local};
    const std::array current{local, local};
    CHECK(!validate_applied_sequence(original, injected, current));
}

void test_grapple_cooldown_default_off_is_idle_and_enabled_state_is_idempotent() {
    using namespace grappling_hook;

    CooldownOverrideLedger ledger;
    CHECK(ledger.begin_world(7));
    CHECK(ledger.next_work(7, true) == CooldownWork::none);

    ledger.set_desired(true);
    CHECK(ledger.next_work(7, true) == CooldownWork::apply);
    CHECK(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(
        7, CooldownApplyOutcome::succeeded,
        {{.objectFullName = L"PalWeaponBase /Game/GrappleA", .originalCooldown = 12.0F},
         {.objectFullName = L"PalWeaponBase /Game/GrappleB", .originalCooldown = 6.0F}}));
    CHECK(ledger.next_work(7, true) == CooldownWork::none);
    CHECK(ledger.records().size() == 2);

    ledger.set_desired(true);
    CHECK(ledger.next_work(7, true) == CooldownWork::none);
}

void test_grapple_target_unavailable_waits_for_explicit_retry() {
    using namespace grappling_hook;

    CooldownOverrideLedger ledger;
    CHECK(ledger.begin_world(7));
    ledger.set_desired(true);
    CHECK(ledger.next_work(7, true) == CooldownWork::apply);
    CHECK(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(7, CooldownApplyOutcome::targetUnavailable));

    CHECK(ledger.phase(7) == CooldownRuntimePhase::waitingForRetry);
    CHECK(ledger.next_work(7, true) == CooldownWork::none);
    CHECK(!ledger.request_retry(8));
    CHECK(ledger.request_retry(7));
    CHECK(ledger.next_work(7, true) == CooldownWork::apply);
}

void test_grapple_terminal_failure_is_not_reenabled_by_retry_or_toggle() {
    using namespace grappling_hook;

    CooldownOverrideLedger ledger;
    CHECK(ledger.begin_world(7));
    ledger.set_desired(true);
    CHECK(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(7, CooldownApplyOutcome::terminalFailure));
    CHECK(ledger.phase(7) == CooldownRuntimePhase::safetyDisabled);
    CHECK(!ledger.request_retry(7));
    ledger.set_desired(false);
    ledger.set_desired(true);
    CHECK(ledger.phase(7) == CooldownRuntimePhase::safetyDisabled);
    CHECK(ledger.next_work(7, true) == CooldownWork::none);

    CHECK(ledger.begin_world(8));
    CHECK(ledger.phase(8) == CooldownRuntimePhase::readyToApply);
    CHECK(ledger.next_work(8, true) == CooldownWork::apply);
}

void test_grapple_gateway_status_classifies_only_missing_target_as_retryable() {
    using namespace grappling_hook;

    CHECK(to_apply_outcome(CooldownGatewayStatus::succeeded) == CooldownApplyOutcome::succeeded);
    CHECK(to_apply_outcome(CooldownGatewayStatus::targetUnavailable) ==
          CooldownApplyOutcome::targetUnavailable);
    CHECK(to_apply_outcome(CooldownGatewayStatus::layoutUnavailable) ==
          CooldownApplyOutcome::terminalFailure);
    CHECK(to_apply_outcome(CooldownGatewayStatus::verificationFailed) ==
          CooldownApplyOutcome::terminalFailure);
}

void test_grapple_apply_readiness_requires_world_callbacks_and_common_inventory() {
    using namespace grappling_hook;

    CHECK(!grapple_apply_ready(false, true, true));
    CHECK(!grapple_apply_ready(true, false, true));
    CHECK(!grapple_apply_ready(true, true, false));
    CHECK(grapple_apply_ready(true, true, true));
}

void test_grapple_readiness_scheduler_polls_only_when_requested_and_at_interval() {
    using namespace grappling_hook;

    CooldownReadinessScheduler scheduler;
    scheduler.begin_world(7);

    CHECK(scheduler.advance(0.0F, 7, true));
    scheduler.complete(7, false);
    CHECK(!scheduler.advance(0.49F, 7, true));
    CHECK(scheduler.advance(0.01F, 7, true));
    scheduler.complete(7, true);
    CHECK(!scheduler.advance(10.0F, 7, true));

    scheduler.request(7);
    CHECK(!scheduler.advance(0.0F, 8, true));
    CHECK(!scheduler.advance(0.0F, 7, false));
    CHECK(scheduler.advance(0.0F, 7, true));

    scheduler.begin_world(8);
    CHECK(!scheduler.advance(0.0F, 7, true));
    CHECK(scheduler.advance(0.0F, 8, true));
}

void test_grapple_target_filter_accepts_only_known_item_ids() {
    CHECK(grappling_hook::is_grappling_item_id("GrapplingGun"));
    CHECK(grappling_hook::is_grappling_item_id("GrapplingGun2"));
    CHECK(grappling_hook::is_grappling_item_id("GrapplingGun5"));
    CHECK(grappling_hook::is_grappling_item_id("GrapplingGun_1"));
    CHECK(!grappling_hook::is_grappling_item_id("AirGrapplingGun"));
    CHECK(!grappling_hook::is_grappling_item_id("AssaultRifle_Default1"));
    CHECK(!grappling_hook::is_grappling_item_id(""));
}

void test_grapple_cooldown_restores_each_original_before_changing_world() {
    using namespace grappling_hook;

    CooldownOverrideLedger ledger;
    CHECK(ledger.begin_world(7));
    ledger.set_desired(true);
    CHECK(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(
        7, CooldownApplyOutcome::succeeded,
        {{.objectFullName = L"PalWeaponBase /Game/GrappleA", .originalCooldown = 12.0F},
         {.objectFullName = L"PalWeaponBase /Game/GrappleB", .originalCooldown = 6.0F}}));

    ledger.set_desired(false);
    CHECK(ledger.next_work(7, true) == CooldownWork::restore);
    CHECK(!ledger.begin_world(8));
    ledger.complete_restore(false);
    CHECK(ledger.next_work(7, true) == CooldownWork::none);

    ledger.complete_restore(true);
    CHECK(ledger.records().empty());
    CHECK(ledger.begin_world(8));
    CHECK(ledger.next_work(8, true) == CooldownWork::none);
}

auto main() -> int {
    test_resource_pool_filters_deduplicates_and_orders();
    test_runtime_state_fails_closed_across_worlds();
    test_hook_capabilities_require_preview_and_consume_paths();
    test_union_plan_appends_only_missing_container_ids();
    test_recorded_injection_removal_preserves_runtime_native_changes();
    test_status_text_reports_partial_support();
    test_disabled_resource_sharing_has_no_runtime_work();
    test_hook_manifest_acquires_before_first_build_and_craft_eligibility();
    test_hook_manifest_keeps_crafting_alive_through_submission();
    test_missing_early_build_acquire_disables_only_building();
    test_hook_manifest_tracks_current_base_context();
    test_missing_current_base_hook_disables_only_building();
    test_resource_toggle_transition_distinguishes_disable_and_accessible_reenable();
    test_reconcile_scheduler_coalesces_events_and_uses_bounded_intervals();
    test_foreground_session_preempts_instead_of_combining_operations();
    test_foreground_session_ignores_stale_touch_and_release();
    test_foreground_crafting_session_expires_only_after_idle_lease();
    test_current_base_state_never_leaks_across_worlds();
    test_resource_exposure_uses_exactly_one_consumer_surface();
    test_applied_sequence_rejects_duplicate_remote_container();
    test_applied_sequence_rejects_injection_of_existing_id();
    test_grapple_cooldown_default_off_is_idle_and_enabled_state_is_idempotent();
    test_grapple_target_unavailable_waits_for_explicit_retry();
    test_grapple_terminal_failure_is_not_reenabled_by_retry_or_toggle();
    test_grapple_gateway_status_classifies_only_missing_target_as_retryable();
    test_grapple_apply_readiness_requires_world_callbacks_and_common_inventory();
    test_grapple_readiness_scheduler_polls_only_when_requested_and_at_interval();
    test_grapple_target_filter_accepts_only_known_item_ids();
    test_grapple_cooldown_restores_each_original_before_changing_world();
    return failures == 0 ? 0 : 1;
}
