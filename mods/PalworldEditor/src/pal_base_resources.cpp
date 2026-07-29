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
#include <Unreal/Hooks.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <base_resource_sharing/hook_manifest.hpp>
#include <base_resource_sharing/pal_base_resources.hpp>
#include <base_resource_sharing/resource_session.hpp>

#include "pal_base_resource_runtime.hpp"

namespace base_resource_sharing {
using namespace RC;
using namespace RC::Unreal;

namespace {
[[nodiscard]] auto object_name(UObject* object) -> std::wstring {
    return object == nullptr ? std::wstring{} : std::wstring{object->GetFullName()};
}

[[nodiscard]] auto find_object_by_full_name(const std::wstring& fullName) -> UObject* {
    if (fullName.empty()) {
        return nullptr;
    }
    const auto separator = fullName.find(L' ');
    const auto objectPath =
        separator == std::wstring::npos ? fullName : fullName.substr(separator + 1);
    return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, objectPath.c_str());
}

[[nodiscard]] auto call_bool(UObject* target, const CharType* functionName, bool& result) -> bool {
    result = false;
    if (target == nullptr) {
        return false;
    }
    auto* function = target->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return false;
    }
    struct Params {
        bool ReturnValue{};
    } params;
    target->ProcessEvent(function, &params);
    result = params.ReturnValue;
    return true;
}

[[nodiscard]] auto read_object_parameter(UFunction* function,
                                         UnrealScriptFunctionCallableContext& context,
                                         const CharType* parameterName) -> UObject* {
    auto* property = function == nullptr ? nullptr
                                         : CastField<FObjectPropertyBase>(function->FindProperty(
                                               FName(parameterName, FNAME_Find)));
    auto* locals = context.TheStack.Locals();
    if (property == nullptr || locals == nullptr || !property->HasAnyPropertyFlags(CPF_Parm)) {
        return nullptr;
    }
    return property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(locals));
}

[[nodiscard]] auto read_object_property(UObject* object, const CharType* propertyName) -> UObject* {
    auto* property =
        object == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(object->GetPropertyByNameInChain(propertyName));
    return property == nullptr
               ? nullptr
               : property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(object));
}

[[nodiscard]] constexpr auto operation_log_name(const ResourceOperation operation) noexcept
    -> const CharType* {
    switch (operation) {
        case ResourceOperation::crafting:
            return STR("crafting");
        case ResourceOperation::building:
            return STR("building");
        case ResourceOperation::repair:
            return STR("repair");
    }
    return STR("unknown");
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
        std::pair<int, int> ids{-1, -1};
    };

    auto set_enabled(const bool enabled) -> void {
        const auto transition =
            decide_resource_toggle(runtime_.enabled(), enabled, runtime_.accessible());
        if (!transition.disableRuntime && !transition.beginAccessibleWorld &&
            runtime_.enabled() == enabled) {
            return;
        }

        if (transition.disableRuntime) {
            restore_or_disable("关闭资源共享");
            sessions_.reset();
            currentBase_.reset();
            scheduler_.reset();
            unregister_resource_hooks();
            catalog_ = {};
            baseCount_ = 0;
            containerCount_ = 0;
            worldContextFullName_.clear();
            runtimeError_.clear();
        }

        runtime_.set_preference(enabled);
        if (transition.beginAccessibleWorld) {
            const auto generation = runtime_.generation();
            sessions_.begin_world(generation);
            currentBase_.begin_world(generation);
            scheduler_.begin_world(generation);
            runtimeError_.clear();
        }
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto on_world_begin(const std::uint64_t generation) -> void {
        restore_or_disable("切换世界");
        sessions_.reset();
        currentBase_.reset();
        scheduler_.reset();
        unregister_resource_hooks();
        worldDisabledErrors_ = {};
        safetyDisabled_ = false;
        catalog_ = {};
        baseCount_ = 0;
        containerCount_ = 0;
        lastCatalogMilliseconds_ = 0.0;
        lastUnionMilliseconds_ = 0.0;
        worldContextFullName_.clear();
        runtime_.begin_world_transition(generation);
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto on_world_ready(const std::uint64_t generation) -> void {
        runtime_.finish_world_transition(generation);
        sessions_.begin_world(generation);
        currentBase_.begin_world(generation);
        scheduler_.begin_world(generation);
        publish_capabilities();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto tick(const float deltaSeconds) -> void {
        const auto generation = runtime_.generation();
        if (!runtime_.enabled() || !runtime_.accessible()) {
            publish_snapshot();
            return;
        }

        const auto sessionTransition = sessions_.advance(deltaSeconds, generation);
        if (sessionTransition.kind == ForegroundTransitionKind::released) {
            restore_or_disable("制作会话空闲");
        }

        const bool idle = !sessions_.active(generation).has_value() && !liveUnion_.active;
        if (scheduler_.advance(deltaSeconds, generation, idle)) {
            auto* worldContext = resolve_world_context(nullptr);
            reconcile_catalog(worldContext);
        }
        publish_snapshot();
    }

    auto ensure_hooks_registered() -> void {
        if (!resource_hooks_required(runtime_.enabled(), runtime_.accessible())) {
            if (!hooks_.empty()) {
                unregister_resource_hooks();
                publish_snapshot();
            }
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
            const auto ids = UObjectGlobals::RegisterHook(
                function,
                [this, function, spec](UnrealScriptFunctionCallableContext& context, void*) {
                    on_hook_pre(function, spec, context);
                },
                [this, function, spec](UnrealScriptFunctionCallableContext& context, void*) {
                    on_hook_post(function, spec, context);
                },
                nullptr);
            if (ids.first >= 0 && ids.second >= 0) {
                hooks_.push_back({.function = function, .spec = spec, .ids = ids});
            }
        }

        rebuild_resolutions();
        publish_capabilities();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto shutdown_hooks() -> void {
        restore_or_disable("卸载 mod");
        sessions_.reset();
        currentBase_.reset();
        scheduler_.reset();
        unregister_resource_hooks();
        runtime_.begin_world_transition(runtime_.generation() + 1);
        catalog_ = {};
        baseCount_ = 0;
        containerCount_ = 0;
        worldContextFullName_.clear();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    [[nodiscard]] auto snapshot() const -> BaseResourceSharingSnapshot {
        const std::lock_guard lock(snapshotMutex_);
        return snapshot_;
    }

private:
    auto remember_world_context(UObject* context) -> void {
        if (context != nullptr) {
            worldContextFullName_ = object_name(context);
        }
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

    [[nodiscard]] auto catalog_ready() const noexcept -> bool {
        return catalog_.generation == runtime_.generation() && catalog_.guildId.valid() &&
               catalog_.error.empty() && !catalog_.plan.ordered.empty();
    }

    auto accept_catalog(detail::ResourceCatalogSnapshot next) -> void {
        catalog_ = std::move(next);
        baseCount_ = catalog_.sameGuildBaseCount;
        containerCount_ = catalog_.plan.ordered.size();
        runtimeError_.clear();
        snapshotDirty_.mark();
    }

    auto reconcile_catalog(UObject* worldContext) -> void {
        const auto generation = runtime_.generation();
        const auto started = std::chrono::steady_clock::now();
        if (liveUnion_.active) {
            scheduler_.complete(false, generation);
            return;
        }

        auto next = detail::discover_catalog(worldContext, generation);
        const bool discovered = next.error.empty();
        scheduler_.complete(discovered, generation);
        if (!discovered) {
            runtimeError_ = next.error;
            catalog_.error = next.error;
            snapshotDirty_.mark();
            return;
        }

        accept_catalog(std::move(next));

        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        lastCatalogMilliseconds_ = elapsed;
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: resource catalog reconciled in {:.3f} ms, bases={}, "
                "containers={}, union={}\n"),
            elapsed, baseCount_, containerCount_, liveUnion_.active);
        snapshotDirty_.mark();
    }

    [[nodiscard]] auto apply_live_union(UObject* worldContext, const ResourceExposurePlan& exposure)
        -> bool {
        if (liveUnion_.active) {
            if (liveUnion_.generation != runtime_.generation() ||
                liveUnion_.guildId != catalog_.guildId) {
                return false;
            }
            if (liveUnion_.exposure == exposure) {
                return true;
            }
            if (!restore_live_union("切换前台材料操作")) {
                return false;
            }
        }
        if (worldContext == nullptr || !catalog_ready()) {
            runtimeError_ = "资源目录尚未安全就绪。";
            snapshotDirty_.mark();
            return false;
        }

        const auto started = std::chrono::steady_clock::now();
        std::string error;
        bool applied{};
        {
            MutationScope mutation{selfMutation_};
            applied = detail::apply_union(worldContext, catalog_, exposure, liveUnion_, error);
        }
        if (!applied) {
            runtimeError_ = std::move(error);
            const bool restoreFailed =
                runtimeError_.find("未能验证据点资源联合已完整恢复") != std::string::npos;
            const bool sequenceInvalid =
                runtimeError_.find("联合序列验证失败") != std::string::npos;
            if (restoreFailed) {
                safetyDisabled_ = true;
                const std::string safetyError =
                    "资源联合回滚未能验证；本世界已禁用制作和建造共享。";
                disable_operation(ResourceOperation::crafting, safetyError);
                disable_operation(ResourceOperation::building, safetyError);
            } else if (sequenceInvalid) {
                safetyDisabled_ = true;
                disable_operation(exposure.operation, runtimeError_);
            }
            snapshotDirty_.mark();
            return false;
        }

        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        lastUnionMilliseconds_ = elapsed;
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: resource union opened in {:.3f} ms, operation={}, surface={}, "
                "injected={}\n"),
            elapsed, static_cast<int>(exposure.operation), static_cast<int>(exposure.surface),
            liveUnion_.entry.has_value() ? liveUnion_.entry->injected.size() : 0);
        runtimeError_.clear();
        snapshotDirty_.mark();
        return true;
    }

    [[nodiscard]] auto ensure_union(UObject* hint, const ResourceExposurePlan& exposure) -> bool {
        remember_world_context(hint);
        if (hint == nullptr || exposure.surface == ResourceConsumerSurface::none ||
            !catalog_ready()) {
            return false;
        }
        return apply_live_union(hint, exposure);
    }

    [[nodiscard]] auto restore_live_union(const std::string_view reason) -> bool {
        if (!liveUnion_.active) {
            return true;
        }
        const auto started = std::chrono::steady_clock::now();
        std::string error;
        bool restored{};
        {
            MutationScope mutation{selfMutation_};
            restored = detail::restore_union(liveUnion_, error);
        }
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        const std::wstring wideReason{reason.begin(), reason.end()};
        Output::send<LogLevel::Verbose>(
            restored ? STR("PalworldEditor: resource union restored in {:.3f} ms ({})\n")
                     : STR("PalworldEditor: resource union restore failed in {:.3f} ms ({})\n"),
            elapsed, wideReason);
        if (!restored) {
            runtimeError_ = std::move(error);
        }
        snapshotDirty_.mark();
        return restored;
    }

    auto restore_or_disable(const std::string_view reason) -> void {
        if (restore_live_union(reason)) {
            return;
        }
        safetyDisabled_ = true;
        const std::string error = "资源联合未能完整恢复；本世界已禁用制作和建造共享。";
        disable_operation(ResourceOperation::crafting, error);
        disable_operation(ResourceOperation::building, error);
    }

    auto update_building_mode(UObject* builder) -> void {
        bool inBuildingMode{};
        if (!call_bool(builder, STR("IsInBuildingMode"), inBuildingMode)) {
            runtimeError_ = "无法读取当前建造模式状态。";
            snapshotDirty_.mark();
            return;
        }
        const auto generation = runtime_.generation();
        if (inBuildingMode) {
            return;
        }
        const auto transition = sessions_.release(ResourceOperation::building, generation);
        if (transition.kind == ForegroundTransitionKind::released) {
            restore_or_disable("退出建造模式");
        }
    }

    [[nodiscard]] auto bootstrap_catalog_for_acquire(UObject* worldContext,
                                                     const ResourceOperation operation) -> bool {
        const auto generation = runtime_.generation();
        const auto capabilityReady = runtime_.capability(operation).available();
        if (!should_bootstrap_catalog(runtime_.enabled(), runtime_.accessible(), capabilityReady,
                                      catalog_ready(), runtime_.generation(), generation)) {
            return catalog_ready();
        }

        scheduler_.request_immediate(generation);
        const bool schedulerTicket = scheduler_.advance(
            0.0F, generation, !sessions_.active(generation).has_value() && !liveUnion_.active);
        auto next = detail::discover_catalog(worldContext, generation);
        const bool discovered = next.error.empty() && next.generation == runtime_.generation();
        if (schedulerTicket) {
            scheduler_.complete(discovered, generation);
        }
        if (!discovered) {
            runtimeError_ =
                next.error.empty() ? "目录 bootstrap 的世界代次已过期。" : std::move(next.error);
            snapshotDirty_.mark();
            return false;
        }

        accept_catalog(std::move(next));
        return true;
    }

    [[nodiscard]] auto ensure_exposure_before_original(UObject* context,
                                                       const ResourceOperation operation) -> bool {
        const auto generation = runtime_.generation();
        if (!runtime_.can_extend(operation, generation)) {
            return false;
        }
        remember_world_context(context);

        const bool bootstrapRequired = !catalog_ready();
        const auto catalogStarted = std::chrono::steady_clock::now();
        if (bootstrapRequired && !bootstrap_catalog_for_acquire(context, operation)) {
            return false;
        }
        const auto catalogElapsed = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - catalogStarted)
                                        .count();
        if (bootstrapRequired) {
            lastCatalogMilliseconds_ = catalogElapsed;
            snapshotDirty_.mark();
        }
        if (generation != runtime_.generation()) {
            return false;
        }

        const auto exposure = make_exposure_plan(operation);
        if (exposure.surface == ResourceConsumerSurface::none) {
            runtimeError_ = "当前材料操作没有可用的单一消费入口。";
            snapshotDirty_.mark();
            return false;
        }
        const auto transition = sessions_.acquire(operation, generation);
        if (transition.kind == ForegroundTransitionKind::none) {
            return false;
        }
        if (transition.kind == ForegroundTransitionKind::preempted &&
            !restore_live_union("前台材料操作抢占")) {
            static_cast<void>(sessions_.release(operation, generation));
            safetyDisabled_ = true;
            const std::string error = "旧材料联合未能完整恢复；本世界已禁用制作和建造共享。";
            disable_operation(ResourceOperation::crafting, error);
            disable_operation(ResourceOperation::building, error);
            return false;
        }
        const auto unionStarted = std::chrono::steady_clock::now();
        if (!ensure_union(context, exposure)) {
            static_cast<void>(sessions_.release(operation, generation));
            restore_or_disable("材料操作会话获取失败");
            if (runtimeError_.empty()) {
                runtimeError_ = "未能建立单一材料消费面；本次菜单保持原版行为。";
            }
            snapshotDirty_.mark();
            return false;
        }
        const auto unionElapsed = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - unionStarted)
                                      .count();
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: material acquire operation={} catalog_ms={:.3f} union_ms={:.3f} "
                "entries={}\n"),
            operation_log_name(operation), catalogElapsed, unionElapsed,
            liveUnion_.entry.has_value() ? 1 : 0);
        return true;
    }

    auto handle_touch(const ResourceOperation operation) -> void {
        static_cast<void>(sessions_.touch(operation, runtime_.generation()));
    }

    auto handle_building_setup_complete() -> void {
        const auto generation = runtime_.generation();
        if (sessions_.active(generation) != ResourceOperation::building || !liveUnion_.active ||
            liveUnion_.generation != generation ||
            liveUnion_.exposure.operation != ResourceOperation::building ||
            liveUnion_.exposure.surface != ResourceConsumerSurface::playerHelper ||
            !liveUnion_.entry.has_value() || !liveUnion_.entry->helperArray) {
            return;
        }

        auto* helper = find_object_by_full_name(liveUnion_.entry->objectFullName);
        auto* onRepContainers =
            helper == nullptr ? nullptr : helper->GetFunctionByNameInChain(STR("OnRep_Containers"));
        if (onRepContainers == nullptr) {
            static_cast<void>(sessions_.release(ResourceOperation::building, generation));
            restore_or_disable("建筑材料观察者刷新失败");
            if (!safetyDisabled_) {
                disable_operation(ResourceOperation::building,
                                  "建筑菜单 Setup 后无法刷新材料观察者。");
            }
            return;
        }

        {
            MutationScope mutation{selfMutation_};
            helper->ProcessEvent(onRepContainers, nullptr);
        }
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: building material observers refreshed after menu setup\n"));
    }

    auto handle_crafting_widget_closed(UObject* widget) -> void {
        const auto generation = runtime_.generation();
        if (sessions_.active(generation) != ResourceOperation::crafting) {
            return;
        }
        auto* parameter = read_object_property(widget, STR("Param"));
        if (!is_convert_item_dispatch_parameter(object_name(parameter))) {
            return;
        }
        const auto transition = sessions_.release(ResourceOperation::crafting, generation);
        if (transition.kind == ForegroundTransitionKind::released) {
            restore_or_disable("制作界面关闭");
        }
    }

    auto handle_structure_changed() -> void {
        scheduler_.request_immediate(runtime_.generation());
    }

    auto handle_base_context(UFunction* function, UnrealScriptFunctionCallableContext& context,
                             const bool entering) -> void {
        auto* baseModel = read_object_parameter(function, context, STR("BaseCampModel"));
        const auto baseId = detail::read_base_id(baseModel);
        if (!baseId.has_value()) {
            disable_operation(ResourceOperation::building,
                              "据点进入/离开回调缺少有效 BaseCampModel.BaseCampId。");
            return;
        }
        const auto generation = runtime_.generation();
        const bool changed = entering ? currentBase_.enter(*baseId, generation)
                                      : currentBase_.exit(*baseId, generation);
        if (!changed && entering) {
            disable_operation(ResourceOperation::building,
                              "当前据点回调与世界代次不一致，已停用建造共享。");
        }
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
                handle_structure_changed();
                break;
            case HookEvent::acquire:
                static_cast<void>(ensure_exposure_before_original(context.Context, spec.operation));
                break;
            case HookEvent::touch:
                handle_touch(spec.operation);
                break;
            case HookEvent::refreshBuilding:
                handle_building_setup_complete();
                break;
            case HookEvent::closeCrafting:
                handle_crafting_widget_closed(context.Context);
                break;
            case HookEvent::updateBuildingMode:
                update_building_mode(context.Context);
                break;
            case HookEvent::enterBase:
                handle_base_context(function, context, true);
                break;
            case HookEvent::exitBase:
                handle_base_context(function, context, false);
                break;
        }
        if (event != HookEvent::touch) {
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

    auto unregister_resource_hooks() -> void {
        for (auto hook = hooks_.rbegin(); hook != hooks_.rend(); ++hook) {
            if (hook->function != nullptr && hook->ids.first >= 0) {
                UObjectGlobals::UnregisterHook(hook->function, hook->ids);
            }
        }
        hooks_.clear();
        resolutions_ = all_hook_resolutions(false);
        capabilitiesGeneration_ = 0;
        nextHookAttempt_ = {};
        publish_capabilities();
        snapshotDirty_.mark();
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
        next.foregroundOperation = sessions_.active(next.worldGeneration);
        next.consumerSurface =
            liveUnion_.active ? liveUnion_.exposure.surface : ResourceConsumerSurface::none;
        next.currentBaseId = currentBase_.current(next.worldGeneration);
        next.lastCatalogMilliseconds = lastCatalogMilliseconds_;
        next.lastUnionMilliseconds = lastUnionMilliseconds_;
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
    ReconcileScheduler scheduler_;
    ForegroundMaterialSession sessions_;
    CurrentBaseState currentBase_;
    detail::ResourceCatalogSnapshot catalog_;
    detail::LiveUnion liveUnion_;
    std::vector<HookResolution> resolutions_{all_hook_resolutions(false)};
    std::vector<HookBinding> hooks_;
    std::array<std::string, 3> worldDisabledErrors_;
    std::uint64_t capabilitiesGeneration_{};
    std::size_t baseCount_{};
    std::size_t containerCount_{};
    std::wstring worldContextFullName_;
    std::string runtimeError_;
    std::chrono::steady_clock::time_point nextHookAttempt_{};
    double lastCatalogMilliseconds_{};
    double lastUnionMilliseconds_{};
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

auto PalBaseResourceBridge::shutdown_hooks() -> void {
    impl_->shutdown_hooks();
}

auto PalBaseResourceBridge::snapshot() const -> BaseResourceSharingSnapshot {
    return impl_->snapshot();
}
}  // namespace base_resource_sharing
