#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <base_resource_sharing/hook_manifest.hpp>
#include <base_resource_sharing/pal_base_resource_runtime.hpp>
#include <base_resource_sharing/pal_base_resources.hpp>
#include <base_resource_sharing/persistent_union.hpp>
#include <common/function_hook_registry.hpp>
#include <common/game_reflection.hpp>

namespace base_resource_sharing {
using pal_game::find_object_by_full_name;
using namespace RC;
using namespace RC::Unreal;

namespace {
auto log_shutdown_error_noexcept(const TCHAR* message) noexcept -> void {
    try {
        Output::send<LogLevel::Error>(message);
    } catch (...) {
        // 关停期间日志设备可能已经不可用；不得因此跳过后续 Hook 注销。
        static_cast<void>(0);
    }
}

[[nodiscard]] auto registration_key(const PersistentUnionEdge& edge)
    -> ConcreteModelRegistrationKey {
    return {.moduleFullName = edge.targetModuleFullName, .ownerMapObjectId = edge.ownerMapObjectId};
}

class MutationScope {
public:
    explicit MutationScope(bool& flag) noexcept : flag_{flag} {
        flag_ = true;
    }

    ~MutationScope() {
        flag_ = false;
    }

    MutationScope(const MutationScope&) = delete;
    auto operator=(const MutationScope&) -> MutationScope& = delete;

private:
    bool& flag_;
};
}  // namespace

class PalBaseResourceBridge::Impl {
public:
    struct HookBinding {
        UFunction* function{};
        HookSpec spec;
    };

    auto set_enabled(const bool enabled) -> void {
        if (runtime_.enabled() == enabled) {
            return;
        }

        runtime_.set_preference(enabled);
        const auto generation = runtime_.generation();
        if (enabled && runtime_.accessible() && !safetyDisabled_) {
            static_cast<void>(unionLifecycle_.request_enable(generation));
            runtimeError_.clear();
            reconcileRequested_ = false;
            waitingForStructure_ = false;
            reset_pending_work();
        } else if (!enabled && runtime_.accessible()) {
            if (unionLifecycle_.request_disable(generation)) {
                prepare_restore_work();
            } else if (unionLifecycle_.phase(generation) == PersistentUnionPhase::off) {
                finalize_disabled_state();
            }
        }
        publish_capabilities();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto on_world_begin(const std::uint64_t generation) -> void {
        restore_all_synchronously("切换世界");
        static_cast<void>(unregister_resource_hooks());
        worldDisabledErrors_ = {};
        safetyDisabled_ = false;
        unionLedger_.clear();
        desiredPlan_ = {};
        unionLifecycle_.begin_world(generation);
        reconcileRequested_ = false;
        waitingForStructure_ = false;
        reset_pending_work();
        catalog_ = {};
        registrationIndex_.clear();
        baseCount_ = 0;
        containerCount_ = 0;
        pendingContainerCount_ = 0;
        lastCatalogMilliseconds_ = 0.0;
        lastSuccessfulCatalogMilliseconds_ = 0.0;
        maximumCatalogMilliseconds_ = 0.0;
        catalogAttemptCount_ = 0;
        worldContextFullName_.clear();
        runtime_.begin_world_transition(generation);
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto on_world_ready(const std::uint64_t generation) -> void {
        runtime_.finish_world_transition(generation);
        unionLifecycle_.begin_world(generation);
        waitingForStructure_ = false;
        if (runtime_.enabled() && !safetyDisabled_) {
            static_cast<void>(unionLifecycle_.request_enable(generation));
        }
        publish_capabilities();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto tick(const float deltaSeconds) -> void {
        static_cast<void>(deltaSeconds);
        if (!runtime_.accessible()) {
            publish_snapshot();
            return;
        }

        const auto phase = unionLifecycle_.phase(runtime_.generation());
        if (phase == PersistentUnionPhase::restoring) {
            process_persistent_work();
        } else if (runtime_.enabled() && !safetyDisabled_ && required_hooks_ready()) {
            if (phase == PersistentUnionPhase::off) {
                static_cast<void>(unionLifecycle_.request_enable(runtime_.generation()));
            }
            const auto currentPhase = unionLifecycle_.phase(runtime_.generation());
            if (currentPhase == PersistentUnionPhase::initializing ||
                currentPhase == PersistentUnionPhase::reconciling) {
                if (!waitingForStructure_) {
                    process_persistent_work();
                }
            }
        }
        publish_snapshot();
    }

    auto ensure_hooks_registered() -> void {
        const bool restoring =
            unionLifecycle_.phase(runtime_.generation()) == PersistentUnionPhase::restoring;
        if (!resource_hooks_required(runtime_.enabled(), runtime_.accessible()) && !restoring) {
            if (!hooks_.empty()) {
                static_cast<void>(unregister_resource_hooks());
                publish_snapshot();
            }
            return;
        }
        if (required_hooks_ready()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < nextHookAttempt_) {
            return;
        }
        nextHookAttempt_ = now + std::chrono::seconds{5};

        for (const auto& spec : palworld_1_0_1_hook_manifest()) {
            if (std::ranges::any_of(
                    hooks_, [&](const auto& hook) { return hook.spec.path == spec.path; })) {
                continue;
            }
            const std::wstring path{spec.path.begin(), spec.path.end()};
            auto* function =
                UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path.c_str());
            if (function == nullptr) {
                continue;
            }
            UnrealScriptFunctionCallable preCallback;
            UnrealScriptFunctionCallable postCallback;
            if (spec.preEvent != HookEvent::none) {
                preCallback = [this, function, spec](UnrealScriptFunctionCallableContext& context,
                                                     void*) {
                    on_hook_pre(function, spec, context);
                };
            }
            if (spec.postEvent != HookEvent::none) {
                postCallback = [this, function, spec](UnrealScriptFunctionCallableContext& context,
                                                      void*) {
                    on_hook_post(function, spec, context);
                };
            }
            if (hookRegistry_.register_hook(function, std::move(preCallback),
                                            std::move(postCallback))) {
                hooks_.push_back({.function = function, .spec = spec});
            }
        }

        rebuild_resolutions();
        publish_capabilities();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto shutdown_hooks() -> bool {
        // Restoration and hook removal are independent cleanup responsibilities. In particular,
        // an allocation or reflection failure while restoring must never leave callbacks pointing
        // into a DLL that UE4SS is about to unload.
        bool allRestored = true;
        try {
            restore_all_synchronously("卸载 mod");
        } catch (...) {
            allRestored = false;
            log_shutdown_error_noexcept(
                STR("PalworldEditor: persistent storage restore threw during shutdown.\n"));
        }
        try {
            // Residue comes from unregister_all's return value: it covers both retained
            // bindings and a stranded script dispatcher, which empty() cannot see.
            const std::size_t residue = unregister_resource_hooks();
            if (residue != 0) {
                allRestored = false;
                log_shutdown_error_noexcept(
                    STR("PalworldEditor: resource hook removal left registrations behind.\n"));
            }
        } catch (...) {
            allRestored = false;
            log_shutdown_error_noexcept(
                STR("PalworldEditor: resource hook removal threw during shutdown.\n"));
        }
        runtime_.begin_world_transition(runtime_.generation() + 1);
        unionLifecycle_.begin_world(runtime_.generation());
        desiredPlan_ = {};
        reset_pending_work();
        baseCount_ = 0;
        containerCount_ = 0;
        pendingContainerCount_ = 0;
        if (unionLedger_.empty()) {
            catalog_ = {};
            registrationIndex_.clear();
            worldContextFullName_.clear();
        } else {
            // EngineTick 异常停用后对象仍可能继续存活；保留精确边账本及其解析上下文，
            // 让随后的游戏线程卸载清理仍能重试，而不是把未完成恢复伪装成成功。
            safetyDisabled_ = true;
            allRestored = false;
        }
        snapshotDirty_.mark();
        publish_snapshot();
        return allRestored;
    }

    [[nodiscard]] auto snapshot() const -> BaseResourceSharingSnapshot {
        const std::lock_guard lock(snapshotMutex_);
        return snapshot_;
    }

private:
    auto remember_world_context(UObject* context) -> void {
        worldContextFullName_ = pal_game::object_full_name(context);
    }

    [[nodiscard]] auto resolve_world_context(UObject* hint) -> UObject* {
        if (hint != nullptr) {
            remember_world_context(hint);
            return hint;
        }
        if (auto* saved = find_object_by_full_name(worldContextFullName_); saved != nullptr) {
            return saved;
        }

        // This is a one-time bootstrap per world. Subsequent reconciliations use StaticFindObject.
        auto* inventory = UObjectGlobals::FindFirstOf(STR("PalPlayerInventoryData"));
        remember_world_context(inventory);
        return inventory;
    }

    auto reset_pending_work() noexcept -> void {
        work_ = {};
        removalIndex_ = 0;
        additionIndex_ = 0;
        workPrepared_ = false;
    }

    auto prepare_restore_work() -> void {
        work_ = {};
        work_.removed.assign(unionLedger_.edges().begin(), unionLedger_.edges().end());
        removalIndex_ = 0;
        additionIndex_ = 0;
        workPrepared_ = true;
        snapshotDirty_.mark();
    }

    auto finalize_disabled_state() -> void {
        desiredPlan_ = {};
        reset_pending_work();
        catalog_ = {};
        registrationIndex_.clear();
        baseCount_ = 0;
        containerCount_ = 0;
        pendingContainerCount_ = 0;
        worldContextFullName_.clear();
        runtimeError_.clear();
        reconcileRequested_ = false;
        waitingForStructure_ = false;
        publish_capabilities();
        snapshotDirty_.mark();
    }

    auto fail_persistent_union(std::string error) -> void {
        runtimeError_ = std::move(error);
        safetyDisabled_ = true;
        reconcileRequested_ = false;
        waitingForStructure_ = false;
        unionLifecycle_.fail(runtime_.generation());
        disable_operation(ResourceOperation::crafting, runtimeError_);
        disable_operation(ResourceOperation::building, runtimeError_);
        reset_pending_work();
        snapshotDirty_.mark();
    }

    [[nodiscard]] auto prepare_reconcile_work() -> bool {
        auto* worldContext = resolve_world_context(nullptr);
        if (worldContext == nullptr) {
            runtimeError_ = "正在等待可用于发现据点目录的游戏上下文。";
            waitingForStructure_ = true;
            snapshotDirty_.mark();
            return false;
        }

        const auto generation = runtime_.generation();
        const auto started = std::chrono::steady_clock::now();
        auto next = detail::discover_catalog(worldContext, generation, unionLedger_.edges());
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        const bool structuralFailure = !next.error.empty() || next.generation != generation;
        record_catalog_attempt(elapsed, !structuralFailure);
        if (structuralFailure) {
            fail_persistent_union(next.error.empty() ? "持久联合目录的世界代次已过期。"
                                                     : std::move(next.error));
            return false;
        }

        accept_catalog(std::move(next));
        auto modules = detail::persistent_modules(catalog_);
        const auto missingEdges = missing_observed_persistent_edges(
            unionLedger_.edges(), catalog_.observedAppliedEdges, modules);
        for (const auto& edge : missingEdges) {
            static_cast<void>(unionLedger_.erase(edge));
        }
        modules = remove_applied_target_edges(std::move(modules), unionLedger_.edges());
        containerCount_ = 0;
        for (const auto& module : modules) {
            containerCount_ += module.containers.size();
        }
        auto plan = make_persistent_union_plan(modules);
        if (!plan.error.empty()) {
            runtimeError_ = "正在等待同公会据点仓储模块完成加载。";
            waitingForStructure_ = true;
            snapshotDirty_.mark();
            return false;
        }

        auto difference = diff_persistent_union(plan.edges, unionLedger_.edges());
        desiredPlan_ = std::move(plan);
        work_ = std::move(difference);
        removalIndex_ = 0;
        additionIndex_ = 0;
        workPrepared_ = true;
        waitingForStructure_ = false;
        runtimeError_.clear();
        snapshotDirty_.mark();
        if (!work_.added.empty() || !work_.removed.empty()) {
            Output::send<LogLevel::Verbose>(
                STR("PalworldEditor: persistent storage graph prepared in {:.3f} ms, bases={}, "
                    "containers={}, add={}, remove={}, pending={}\n"),
                elapsed, baseCount_, containerCount_, work_.added.size(), work_.removed.size(),
                pendingContainerCount_);
        }
        return true;
    }

    auto complete_persistent_work() -> void {
        const auto generation = runtime_.generation();
        const auto phase = unionLifecycle_.phase(generation);
        if (phase == PersistentUnionPhase::restoring) {
            if (!unionLedger_.empty()) {
                fail_persistent_union("持久联合恢复结束后账本仍非空。");
                return;
            }
            static_cast<void>(unionLifecycle_.complete_restore(generation));
            finalize_disabled_state();
        } else {
            static_cast<void>(unionLifecycle_.complete_apply(generation));
            if (reconcileRequested_) {
                reconcileRequested_ = false;
                static_cast<void>(unionLifecycle_.invalidate(generation));
            }
            publish_capabilities();
            snapshotDirty_.mark();
        }
        reset_pending_work();
    }

    auto process_persistent_work() -> void {
        const auto generation = runtime_.generation();
        const auto phase = unionLifecycle_.phase(generation);
        if (!workPrepared_) {
            if (phase == PersistentUnionPhase::restoring) {
                prepare_restore_work();
            } else if (!prepare_reconcile_work()) {
                return;
            }
        }

        auto* worldContext = resolve_world_context(nullptr);
        if (worldContext == nullptr && phase != PersistentUnionPhase::restoring) {
            fail_persistent_union("执行持久联合时游戏上下文已经失效。");
            return;
        }

        PersistentUnionWorkBudget budget;
        const auto started = std::chrono::steady_clock::now();
        const auto within_budget = [&] {
            return budget.can_process(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started));
        };

        while (removalIndex_ < work_.removed.size() && within_budget()) {
            const auto edge = work_.removed[removalIndex_++];
            detail::PersistentEdgeMutationResult result;
            {
                MutationScope mutation{selfMutation_};
                result = detail::remove_persistent_edge(worldContext, catalog_, edge);
            }
            budget.record_operation();
            if (!result) {
                if (result.mutation == PersistentEdgeMutation::removed) {
                    static_cast<void>(unionLedger_.erase(edge));
                    static_cast<void>(registrationIndex_.erase(registration_key(edge)));
                }
                fail_persistent_union(std::move(result.error));
                return;
            }
            static_cast<void>(unionLedger_.erase(edge));
            static_cast<void>(registrationIndex_.erase(registration_key(edge)));
        }

        while (additionIndex_ < work_.added.size() && within_budget()) {
            const auto& edge = work_.added[additionIndex_++];
            detail::PersistentEdgeMutationResult result;
            {
                MutationScope mutation{selfMutation_};
                result = detail::apply_persistent_edge(worldContext, catalog_, edge);
            }
            budget.record_operation();
            if (!result) {
                if (result.mutation == PersistentEdgeMutation::added) {
                    static_cast<void>(unionLedger_.record(edge));
                    static_cast<void>(registrationIndex_.record(registration_key(edge)));
                }
                fail_persistent_union(std::move(result.error));
                return;
            }
            if (result.mutation == PersistentEdgeMutation::added) {
                static_cast<void>(unionLedger_.record(edge));
            }
            static_cast<void>(registrationIndex_.record(registration_key(edge)));
        }

        snapshotDirty_.mark();
        if (removalIndex_ == work_.removed.size() && additionIndex_ == work_.added.size()) {
            complete_persistent_work();
        }
    }

    auto restore_all_synchronously(const std::string_view reason) -> void {
        if (unionLedger_.empty()) {
            return;
        }
        auto* worldContext = resolve_world_context(nullptr);
        const auto edges = std::vector<PersistentUnionEdge>{unionLedger_.edges().begin(),
                                                            unionLedger_.edges().end()};
        const auto started = std::chrono::steady_clock::now();
        std::string firstError;
        for (const auto& edge : edges) {
            detail::PersistentEdgeMutationResult result;
            {
                MutationScope mutation{selfMutation_};
                result = detail::remove_persistent_edge(worldContext, catalog_, edge);
            }
            if (result) {
                static_cast<void>(unionLedger_.erase(edge));
            } else {
                if (result.mutation == PersistentEdgeMutation::removed) {
                    static_cast<void>(unionLedger_.erase(edge));
                }
                if (firstError.empty()) {
                    firstError = std::move(result.error);
                }
            }
        }
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        const std::wstring wideReason{reason.begin(), reason.end()};
        const bool restored = unionLedger_.empty() && firstError.empty();
        Output::send<LogLevel::Verbose>(
            restored
                ? STR("PalworldEditor: persistent storage graph restored in {:.3f} ms ({})\n")
                : STR("PalworldEditor: persistent storage graph restore incomplete in {:.3f} ms "
                      "({})\n"),
            elapsed, wideReason);
        if (!restored) {
            fail_persistent_union(firstError.empty() ? "持久联合未能完整恢复。" : firstError);
        }
    }

    auto handle_ensure_persistent_union(UObject* context) -> void {
        remember_world_context(context);
        if (waitingForStructure_) {
            waitingForStructure_ = false;
            reset_pending_work();
        }
        if (runtime_.enabled() && !safetyDisabled_ &&
            unionLifecycle_.phase(runtime_.generation()) == PersistentUnionPhase::off) {
            static_cast<void>(unionLifecycle_.request_enable(runtime_.generation()));
            snapshotDirty_.mark();
        }
    }

    [[nodiscard]] auto validate_persistent_union(const ResourceOperation operation) -> bool {
        const auto generation = runtime_.generation();
        return runtime_.can_extend(operation, generation) &&
               unionLifecycle_.phase(generation) == PersistentUnionPhase::ready;
    }

    auto accept_catalog(detail::ResourceCatalogSnapshot next) -> void {
        catalog_ = std::move(next);
        std::vector<std::wstring> moduleNames;
        moduleNames.reserve(catalog_.modules.size());
        for (const auto& module : catalog_.modules) {
            moduleNames.push_back(module.objectFullName);
        }
        registrationIndex_.reset(moduleNames, catalog_.ignoredModuleNames, catalog_.registrations,
                                 catalog_.ignoredRegistrations);
        baseCount_ = catalog_.sameGuildBaseCount;
        containerCount_ = catalog_.plan.ordered.size();
        pendingContainerCount_ = catalog_.pendingContainerCount;
        runtimeError_.clear();
        snapshotDirty_.mark();
    }

    auto record_catalog_attempt(const double elapsed, const bool successful) -> void {
        lastCatalogMilliseconds_ = elapsed;
        maximumCatalogMilliseconds_ = std::max(maximumCatalogMilliseconds_, elapsed);
        ++catalogAttemptCount_;
        if (successful) {
            lastSuccessfulCatalogMilliseconds_ = elapsed;
        }
    }

    [[nodiscard]] auto concrete_model_membership(UFunction* function,
                                                 UnrealScriptFunctionCallableContext& context) const
        -> ConcreteModelRegistrationMembership {
        if (!catalog_.initialized || function == nullptr || context.Context == nullptr ||
            context.TheStack.Locals() == nullptr) {
            return ConcreteModelRegistrationMembership::unknown;
        }

        auto* modelProperty = CastField<FObjectPropertyBase>(
            function->FindProperty(FName(STR("ConcreteModel"), FNAME_Find)));
        auto* concreteModel =
            modelProperty == nullptr
                ? nullptr
                : modelProperty->GetObjectPropertyValue(
                      modelProperty->ContainerPtrToValuePtr<void>(context.TheStack.Locals()));
        auto* instanceIdProperty =
            concreteModel == nullptr
                ? nullptr
                : CastField<FStructProperty>(
                      concreteModel->GetPropertyByNameInChain(STR("InstanceId")));
        if (!pal_game::matches_struct_identity(instanceIdProperty, STR("Guid"), sizeof(FGuid))) {
            return ConcreteModelRegistrationMembership::unknown;
        }

        FGuid instanceId{};
        instanceIdProperty->CopyCompleteValue(
            &instanceId, instanceIdProperty->ContainerPtrToValuePtr<void>(concreteModel));
        const GuidKey ownerId{{instanceId.A, instanceId.B, instanceId.C, instanceId.D}};
        if (!ownerId.valid()) {
            return ConcreteModelRegistrationMembership::unknown;
        }

        return registrationIndex_.membership(
            {.moduleFullName = pal_game::object_full_name(context.Context),
             .ownerMapObjectId = ownerId});
    }

    auto handle_structure_changed(const StructureChangeSource source,
                                  const ConcreteModelRegistrationMembership membership) -> void {
        if (!runtime_.enabled() || safetyDisabled_ ||
            !should_forward_structure_change(source, waitingForStructure_, membership)) {
            return;
        }

        const auto generation = runtime_.generation();
        switch (structure_change_action(unionLifecycle_.phase(generation))) {
            case PersistentStructureChangeAction::ignore:
                return;
            case PersistentStructureChangeAction::startReconcile:
                if (unionLifecycle_.invalidate(generation)) {
                    reset_pending_work();
                }
                break;
            case PersistentStructureChangeAction::queueFollowUp:
                // A notification that only unblocks initial discovery does not require another
                // pass. Once work is prepared, any number of further notifications collapse into
                // one follow-up reconcile after the current diff reaches a stable ledger.
                if (workPrepared_) {
                    reconcileRequested_ = true;
                }
                break;
        }

        waitingForStructure_ = false;
        publish_capabilities();
        snapshotDirty_.mark();
    }

    auto dispatch_hook(UFunction* function, const HookPhase phase, const HookSpec& spec,
                       UnrealScriptFunctionCallableContext& context) -> void {
        if (selfMutation_ || !runtime_.accessible()) {
            return;
        }
        const auto event = event_for_phase(spec, phase);
        if (event == HookEvent::none) {
            return;
        }

        switch (event) {
            case HookEvent::none:
                break;
            case HookEvent::structureChanged:
                if (!runtime_.enabled() || safetyDisabled_) {
                    break;
                }
                handle_structure_changed(
                    spec.structureSource,
                    spec.structureSource == StructureChangeSource::concreteModelAvailable ||
                            spec.structureSource == StructureChangeSource::concreteModelUnavailable
                        ? concrete_model_membership(function, context)
                        : ConcreteModelRegistrationMembership::unknown);
                break;
            case HookEvent::ensurePersistentUnion:
                handle_ensure_persistent_union(context.Context);
                break;
            case HookEvent::validatePersistentUnion:
                static_cast<void>(validate_persistent_union(spec.operation));
                break;
        }
        if (event != HookEvent::structureChanged) {
            snapshotDirty_.mark();
        }
    }

    auto on_hook_pre(UFunction* function, const HookSpec& spec,
                     UnrealScriptFunctionCallableContext& context) -> void {
        dispatch_hook(function, HookPhase::pre, spec, context);
    }

    auto on_hook_post(UFunction* function, const HookSpec& spec,
                      UnrealScriptFunctionCallableContext& context) -> void {
        dispatch_hook(function, HookPhase::post, spec, context);
    }

    /** @return 注销失败后仍保留的绑定/分发器残留数（含脚本分发器滞留）。 */
    auto unregister_resource_hooks() -> std::size_t {
        const std::size_t residue = hookRegistry_.unregister_all();
        hooks_.clear();
        resolutions_ = all_hook_resolutions(false);
        capabilitiesGeneration_ = 0;
        nextHookAttempt_ = {};
        publish_capabilities();
        snapshotDirty_.mark();
        return residue;
    }

    auto rebuild_resolutions() -> void {
        resolutions_ = all_hook_resolutions(false);
        for (const auto& hook : hooks_) {
            mark_resolved(resolutions_, hook.spec.path);
        }
    }

    [[nodiscard]] auto required_hooks_ready() const -> bool {
        const auto capabilities = evaluate_capabilities(resolutions_);
        return capabilities[operation_index(ResourceOperation::crafting)].available() &&
               capabilities[operation_index(ResourceOperation::building)].available();
    }

    auto publish_capabilities() -> void {
        auto capabilities = evaluate_capabilities(resolutions_);
        const auto phase = unionLifecycle_.phase(runtime_.generation());
        if (runtime_.enabled() && runtime_.accessible() && required_hooks_ready() &&
            phase != PersistentUnionPhase::ready) {
            std::string stageError;
            switch (phase) {
                case PersistentUnionPhase::off:
                case PersistentUnionPhase::initializing:
                    stageError = "持久资源联合正在初始化。";
                    break;
                case PersistentUnionPhase::reconciling:
                    stageError = "持久资源联合正在校准。";
                    break;
                case PersistentUnionPhase::restoring:
                    stageError = "持久资源联合正在恢复原版登记。";
                    break;
                case PersistentUnionPhase::failed:
                    stageError = runtimeError_.empty() ? "持久资源联合已安全停用。" : runtimeError_;
                    break;
                case PersistentUnionPhase::ready:
                    break;
            }
            capabilities[operation_index(ResourceOperation::crafting)] = {.error = stageError};
            capabilities[operation_index(ResourceOperation::building)] = {.error = stageError};
        }
        for (std::size_t index{}; index < capabilities.size(); ++index) {
            if (!worldDisabledErrors_[index].empty()) {
                capabilities[index] = {.error = worldDisabledErrors_[index]};
            }
            runtime_.set_capability(static_cast<ResourceOperation>(index), capabilities[index]);
        }
        capabilitiesGeneration_ = runtime_.generation();
        snapshotDirty_.mark();
    }

    auto disable_operation(const ResourceOperation operation, std::string error) -> void {
        worldDisabledErrors_[operation_index(operation)] = std::move(error);
        runtime_.set_capability(
            operation, CapabilityState{.error = worldDisabledErrors_[operation_index(operation)]});
        runtimeError_ = runtime_.capability(operation).error;
        snapshotDirty_.mark();
    }

    auto publish_snapshot() -> void {
        if (!snapshotDirty_.consume()) {
            return;
        }
        BaseResourceSharingSnapshot next;
        next.enabled = runtime_.enabled();
        next.worldAccessible = runtime_.accessible();
        next.worldGeneration = runtime_.generation();
        next.baseCount = baseCount_;
        next.containerCount = containerCount_;
        next.pendingContainerCount = pendingContainerCount_;
        next.persistentPhase = unionLifecycle_.phase(next.worldGeneration);
        next.appliedEdgeCount = unionLedger_.edges().size();
        next.pendingEdgeCount = workPrepared_ ? (work_.removed.size() - removalIndex_) +
                                                    (work_.added.size() - additionIndex_)
                                              : 0;
        next.consumerSurface = next.persistentPhase == PersistentUnionPhase::ready
                                   ? ResourceConsumerSurface::guildBaseModules
                                   : ResourceConsumerSurface::none;
        next.lastCatalogMilliseconds = lastCatalogMilliseconds_;
        next.lastSuccessfulCatalogMilliseconds = lastSuccessfulCatalogMilliseconds_;
        next.maximumCatalogMilliseconds = maximumCatalogMilliseconds_;
        next.catalogAttemptCount = catalogAttemptCount_;
        next.safetyDisabled = safetyDisabled_;
        for (std::size_t index{}; index < next.capabilities.size(); ++index) {
            next.capabilities[index] = runtime_.capability(static_cast<ResourceOperation>(index));
        }
        next.status = format_status(
            {.enabled = next.enabled,
             .worldAccessible = next.worldAccessible,
             .detectingCapabilities = next.worldAccessible && !required_hooks_ready(),
             .baseCount = next.baseCount,
             .containerCount = next.containerCount,
             .pendingContainerCount = next.pendingContainerCount,
             .craftingAvailable =
                 next.capabilities[operation_index(ResourceOperation::crafting)].available(),
             .buildingAvailable =
                 next.capabilities[operation_index(ResourceOperation::building)].available(),
             .repairAvailable =
                 next.capabilities[operation_index(ResourceOperation::repair)].available(),
             .craftingError = next.capabilities[operation_index(ResourceOperation::crafting)].error,
             .buildingError = next.capabilities[operation_index(ResourceOperation::building)].error,
             .repairError = next.capabilities[operation_index(ResourceOperation::repair)].error,
             .runtimeError = runtimeError_});

        const std::lock_guard lock(snapshotMutex_);
        snapshot_ = std::move(next);
    }

    RuntimeState runtime_;
    detail::ResourceCatalogSnapshot catalog_;
    ConcreteModelRegistrationIndex registrationIndex_;
    PersistentUnionLifecycle unionLifecycle_;
    PersistentUnionLedger unionLedger_;
    PersistentUnionPlan desiredPlan_;
    PersistentUnionDiff work_;
    std::size_t removalIndex_{};
    std::size_t additionIndex_{};
    bool workPrepared_{};
    bool reconcileRequested_{};
    bool waitingForStructure_{};
    std::vector<HookResolution> resolutions_{all_hook_resolutions(false)};
    std::vector<HookBinding> hooks_;
    pal_game::FunctionHookRegistry hookRegistry_{STR("BaseResourceScript")};
    std::array<std::string, 3> worldDisabledErrors_;
    std::uint64_t capabilitiesGeneration_{};
    std::size_t baseCount_{};
    std::size_t containerCount_{};
    std::size_t pendingContainerCount_{};
    std::wstring worldContextFullName_;
    std::string runtimeError_;
    std::chrono::steady_clock::time_point nextHookAttempt_{};
    double lastCatalogMilliseconds_{};
    double lastSuccessfulCatalogMilliseconds_{};
    double maximumCatalogMilliseconds_{};
    std::size_t catalogAttemptCount_{};
    bool selfMutation_{};
    bool safetyDisabled_{};
    SnapshotDirtyFlag snapshotDirty_;
    mutable std::mutex snapshotMutex_;
    BaseResourceSharingSnapshot snapshot_;
};

PalBaseResourceBridge::PalBaseResourceBridge() : impl_{std::make_unique<Impl>()} {}
PalBaseResourceBridge::~PalBaseResourceBridge() = default;
PalBaseResourceBridge::PalBaseResourceBridge(PalBaseResourceBridge&&) noexcept = default;
auto PalBaseResourceBridge::operator=(PalBaseResourceBridge&&) noexcept
    -> PalBaseResourceBridge& = default;

auto PalBaseResourceBridge::set_enabled(const bool enabled) -> void {
    impl_->set_enabled(enabled);
}

auto PalBaseResourceBridge::on_world_begin(const std::uint64_t generation) -> void {
    impl_->on_world_begin(generation);
}

auto PalBaseResourceBridge::on_world_ready(const std::uint64_t generation) -> void {
    impl_->on_world_ready(generation);
}

auto PalBaseResourceBridge::tick(const float deltaSeconds) -> void {
    impl_->tick(deltaSeconds);
}

auto PalBaseResourceBridge::ensure_hooks_registered() -> void {
    impl_->ensure_hooks_registered();
}

auto PalBaseResourceBridge::shutdown_hooks() -> bool {
    return impl_->shutdown_hooks();
}

auto PalBaseResourceBridge::snapshot() const -> BaseResourceSharingSnapshot {
    return impl_->snapshot();
}
}  // namespace base_resource_sharing
