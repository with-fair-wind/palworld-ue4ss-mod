#include <array>
#include <iostream>
#include <vector>

#include <base_resource_sharing/current_base_resolution.hpp>
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
        .pendingContainerCount = 2,
        .craftingAvailable = true,
        .buildingAvailable = true,
        .repairAvailable = false,
        .repairError = "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。",
    };
    const auto text = format_status(status);
    CHECK(text.find("3 个据点") != std::string::npos);
    CHECK(text.find("12 个资源容器") != std::string::npos);
    CHECK(text.find("2 个容器暂未加载") != std::string::npos);
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
    const auto buildList = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalUIBuildModel:GetBuildObjectDataArrayForUIDisplay"},
        &HookSpec::path);
    CHECK(buildList != hooks.end());
    CHECK(event_for_phase(*buildList, HookPhase::pre) == HookEvent::acquire);
    CHECK(buildList->requirement == HookRequirement::required);

    const auto buildEligibility = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject"},
        &HookSpec::path);
    CHECK(buildEligibility != hooks.end());
    CHECK(event_for_phase(*buildEligibility, HookPhase::pre) == HookEvent::acquire);
    CHECK(buildEligibility->requirement == HookRequirement::required);

    const auto buildSetup = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalUIInGameMainMenuBuildModel:Setup"},
        &HookSpec::path);
    CHECK(buildSetup != hooks.end());
    CHECK(event_for_phase(*buildSetup, HookPhase::pre) == HookEvent::acquire);
    CHECK(event_for_phase(*buildSetup, HookPhase::post) == HookEvent::refreshBuilding);
    CHECK(buildSetup->requirement == HookRequirement::required);

    const auto craftInitialize = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalUIConvertItemModel:Initialize"}, &HookSpec::path);
    CHECK(craftInitialize != hooks.end());
    CHECK(event_for_phase(*craftInitialize, HookPhase::pre) == HookEvent::acquire);
    CHECK(event_for_phase(*craftInitialize, HookPhase::post) == HookEvent::none);
    CHECK(craftInitialize->requirement == HookRequirement::required);
}

void test_hook_manifest_contains_only_exact_foreground_hooks() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    CHECK(hooks.size() == 8);
    for (const auto& hook : hooks) {
        CHECK(hook.preEvent != HookEvent::structureChanged);
        CHECK(hook.postEvent != HookEvent::structureChanged);
        CHECK(hook.preEvent != HookEvent::enterBase);
        CHECK(hook.preEvent != HookEvent::exitBase);
    }
}

void test_hook_registration_stops_polling_after_the_minimal_manifest_is_complete() {
    using namespace base_resource_sharing;

    CHECK(!hook_registration_complete(0));
    CHECK(!hook_registration_complete(7));
    CHECK(hook_registration_complete(8));
}

void test_hook_backend_avoids_the_generic_full_name_dispatcher() {
    using namespace base_resource_sharing;

    CHECK(select_resource_hook_backend(true, false, true) == ResourceHookBackend::nativeFunction);
    CHECK(select_resource_hook_backend(true, true, false) == ResourceHookBackend::scriptFunction);
    CHECK(select_resource_hook_backend(false, false, false) == ResourceHookBackend::unsupported);
    CHECK(select_resource_hook_backend(true, false, false) == ResourceHookBackend::unsupported);
    CHECK(select_resource_hook_backend(true, true, true) == ResourceHookBackend::unsupported);
}

void test_hook_manifest_validates_live_unions_before_original_consumption() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    const auto startProduction = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalUIConvertItemModel:StartProduction"},
        &HookSpec::path);
    CHECK(startProduction != hooks.end());
    CHECK(event_for_phase(*startProduction, HookPhase::pre) == HookEvent::validate);
    CHECK(startProduction->requirement == HookRequirement::required);

    const auto requestBuild = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalNetworkPlayerComponent:RequestBuild_ToServer"},
        &HookSpec::path);
    CHECK(requestBuild != hooks.end());
    CHECK(event_for_phase(*requestBuild, HookPhase::pre) == HookEvent::validate);
    CHECK(requestBuild->requirement == HookRequirement::required);
}

void test_hook_manifest_releases_crafting_when_convert_widget_closes() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    const auto closed = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalUserWidget:OnClosed"}, &HookSpec::path);
    CHECK(closed != hooks.end());
    if (closed != hooks.end()) {
        CHECK(event_for_phase(*closed, HookPhase::pre) == HookEvent::closeCrafting);
        CHECK(event_for_phase(*closed, HookPhase::post) == HookEvent::none);
        CHECK(closed->requirement == HookRequirement::required);
    }

    CHECK(is_convert_item_dispatch_parameter(
        L"PalHUDDispatchParameter_ConvertItem /Engine/Transient.Param_0"));
    CHECK(!is_convert_item_dispatch_parameter(
        L"PalHUDDispatchParameter_ItemChest /Engine/Transient.Param_0"));
    CHECK(!is_convert_item_dispatch_parameter(
        L"PalHUDDispatchParameter_ItemChest /Game/PalHUDDispatchParameter_ConvertItem"));
}

void test_missing_early_build_acquire_disables_only_building() {
    using namespace base_resource_sharing;

    auto resolved = all_hook_resolutions(true);
    for (auto& resolution : resolved) {
        if (resolution.spec.path == "/Script/Pal.PalUIInGameMainMenuBuildModel:Setup") {
            resolution.resolved = false;
        }
    }
    const auto capabilities = evaluate_capabilities(resolved);
    CHECK(!capabilities[operation_index(ResourceOperation::building)].available());
    CHECK(capabilities[operation_index(ResourceOperation::crafting)].available());
    CHECK(!capabilities[operation_index(ResourceOperation::repair)].available());
}

void test_hook_manifest_does_not_track_current_base_context() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    const auto enter = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalBuilderComponent:OnEnterBaseCamp"},
        &HookSpec::path);
    const auto exit = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalBuilderComponent:OnExitBaseCamp"}, &HookSpec::path);
    CHECK(enter == hooks.end());
    CHECK(exit == hooks.end());
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

void test_on_demand_catalog_state_coalesces_invalidations_without_idle_work() {
    using namespace base_resource_sharing;

    OnDemandCatalogState state;
    state.begin_world(7);
    CHECK(state.invalidated(7));
    CHECK(!state.invalidate(7));
    CHECK(!state.invalidate(7));
    state.complete(CatalogReconcileOutcome::complete, 7);
    CHECK(!state.invalidated(7));
    CHECK(state.invalidate(7));
    CHECK(!state.invalidate(7));
    CHECK(state.invalidated(7));
    CHECK(!state.invalidate(8));
    CHECK(!state.invalidated(8));
    state.reset();
    CHECK(!state.invalidated(7));
}

void test_catalog_discovery_runs_once_only_for_a_new_foreground_session() {
    using namespace base_resource_sharing;

    CHECK(should_discover_catalog(ForegroundTransitionKind::acquired));
    CHECK(should_discover_catalog(ForegroundTransitionKind::preempted));
    CHECK(!should_discover_catalog(ForegroundTransitionKind::refreshed));
    CHECK(!should_discover_catalog(ForegroundTransitionKind::released));
    CHECK(!should_discover_catalog(ForegroundTransitionKind::none));
}

void test_catalog_attempt_classifies_unloaded_containers_as_partial() {
    using namespace base_resource_sharing;

    CHECK(classify_catalog_attempt(false, 0) == CatalogReconcileOutcome::complete);
    CHECK(classify_catalog_attempt(false, 3) == CatalogReconcileOutcome::partial);
    CHECK(classify_catalog_attempt(true, 0) == CatalogReconcileOutcome::structuralFailure);
    CHECK(classify_catalog_attempt(true, 3) == CatalogReconcileOutcome::structuralFailure);
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

void test_foreground_crafting_session_remains_active_until_explicit_release() {
    using namespace base_resource_sharing;

    ForegroundMaterialSession sessions;
    sessions.begin_world(7);
    static_cast<void>(sessions.acquire(ResourceOperation::crafting, 7));
    CHECK(sessions.advance(60.0F, 7).kind == ForegroundTransitionKind::none);
    CHECK(sessions.active(7) == ResourceOperation::crafting);
    const auto closed = sessions.release(ResourceOperation::crafting, 7);
    CHECK(closed.kind == ForegroundTransitionKind::released);
    CHECK(closed.previous == ResourceOperation::crafting);
    CHECK(!sessions.active(7).has_value());
}

void test_building_inventory_refresh_is_consumed_once_per_foreground_session() {
    using namespace base_resource_sharing;

    ForegroundMaterialSession sessions;
    sessions.begin_world(7);
    static_cast<void>(sessions.acquire(ResourceOperation::building, 7));
    CHECK(sessions.building_inventory_refresh_needed(7));
    CHECK(sessions.complete_building_inventory_refresh(7));
    CHECK(!sessions.building_inventory_refresh_needed(7));
    CHECK(!sessions.complete_building_inventory_refresh(7));

    static_cast<void>(sessions.acquire(ResourceOperation::building, 7));
    CHECK(!sessions.building_inventory_refresh_needed(7));

    static_cast<void>(sessions.acquire(ResourceOperation::crafting, 7));
    CHECK(!sessions.building_inventory_refresh_needed(7));
    static_cast<void>(sessions.acquire(ResourceOperation::building, 7));
    CHECK(sessions.building_inventory_refresh_needed(7));

    CHECK(!sessions.complete_building_inventory_refresh(8));
    CHECK(sessions.building_inventory_refresh_needed(7));
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

    state.begin_world(9);
    CHECK(state.observe(baseA, 9));
    CHECK(state.current(9) == baseA);
    CHECK(state.observe(std::nullopt, 9));
    CHECK(!state.current(9).has_value());
    CHECK(!state.observe(baseB, 8));
}

void test_current_base_resolution_uses_native_inside_base_route() {
    using namespace base_resource_sharing;
    using Names = CurrentBaseReflectionNames<char>;

    CHECK(Names::controllerPawnFunction == "K2_GetPawn");
    CHECK(Names::insideComponentProperty == "InsideBaseCampCheckComponent");
    CHECK(Names::insideBaseModelFunction == "GetInsideBaseCampModel");
    CHECK(Names::baseIdProperty == "BaseCampId");
    CHECK(!kAllowsNearestBaseFallback);

    const GuidKey current{{7, 0, 0, 0}};
    CHECK(accept_current_base(current, true) == current);
    CHECK(!accept_current_base(current, false).has_value());
    CHECK(!accept_current_base(GuidKey{}, true).has_value());
}

void test_resource_exposure_uses_exactly_one_consumer_surface() {
    using namespace base_resource_sharing;

    const GuidKey currentBase{{7, 0, 0, 0}};
    const auto crafting = make_exposure_plan(ResourceOperation::crafting, currentBase);
    CHECK(crafting.surface == ResourceConsumerSurface::playerHelper);
    CHECK(crafting.targetBaseId == currentBase);

    const auto building = make_exposure_plan(ResourceOperation::building, currentBase);
    CHECK(building.surface == ResourceConsumerSurface::currentBaseModule);
    CHECK(building.targetBaseId == currentBase);

    CHECK(make_exposure_plan(ResourceOperation::crafting).surface == ResourceConsumerSurface::none);
    CHECK(make_exposure_plan(ResourceOperation::building).surface == ResourceConsumerSurface::none);
    CHECK(make_exposure_plan(ResourceOperation::repair).surface == ResourceConsumerSurface::none);
}

void test_building_inventory_refresh_targets_only_the_build_model() {
    using namespace base_resource_sharing;

    const GuidKey currentBase{{7, 0, 0, 0}};
    const auto building = make_exposure_plan(ResourceOperation::building, currentBase);
    CHECK(select_building_inventory_refresh_target(true, building) ==
          BuildingInventoryRefreshTarget::buildModel);
    CHECK(select_building_inventory_refresh_target(false, building) ==
          BuildingInventoryRefreshTarget::none);
    CHECK(select_building_inventory_refresh_target(
              true, make_exposure_plan(ResourceOperation::crafting, currentBase)) ==
          BuildingInventoryRefreshTarget::none);
}

void test_current_base_containers_are_not_injected_into_another_consumer_surface() {
    using namespace base_resource_sharing;

    const GuidKey currentBase{{7, 0, 0, 0}};
    const GuidKey remoteBase{{8, 0, 0, 0}};
    const GuidKey currentContainer{{70, 0, 0, 0}};
    const GuidKey remoteContainer{{80, 0, 0, 0}};
    const std::array descriptors{
        ContainerDescriptor{.baseId = currentBase, .containerId = currentContainer},
        ContainerDescriptor{.baseId = remoteBase, .containerId = remoteContainer},
    };

    const auto selected = select_shared_container_ids(descriptors, currentBase);
    CHECK(selected.size() == 1);
    CHECK(selected.front() == remoteContainer);
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
    test_hook_manifest_contains_only_exact_foreground_hooks();
    test_hook_registration_stops_polling_after_the_minimal_manifest_is_complete();
    test_hook_backend_avoids_the_generic_full_name_dispatcher();
    test_hook_manifest_validates_live_unions_before_original_consumption();
    test_hook_manifest_releases_crafting_when_convert_widget_closes();
    test_missing_early_build_acquire_disables_only_building();
    test_hook_manifest_does_not_track_current_base_context();
    test_resource_toggle_transition_distinguishes_disable_and_accessible_reenable();
    test_on_demand_catalog_state_coalesces_invalidations_without_idle_work();
    test_catalog_discovery_runs_once_only_for_a_new_foreground_session();
    test_catalog_attempt_classifies_unloaded_containers_as_partial();
    test_foreground_session_preempts_instead_of_combining_operations();
    test_foreground_session_ignores_stale_touch_and_release();
    test_foreground_crafting_session_remains_active_until_explicit_release();
    test_building_inventory_refresh_is_consumed_once_per_foreground_session();
    test_current_base_state_never_leaks_across_worlds();
    test_current_base_resolution_uses_native_inside_base_route();
    test_resource_exposure_uses_exactly_one_consumer_surface();
    test_building_inventory_refresh_targets_only_the_build_model();
    test_current_base_containers_are_not_injected_into_another_consumer_surface();
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
