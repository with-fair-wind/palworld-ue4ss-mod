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
            sessions_.reset();
            scheduler_.reset();
            restore_or_disable("关闭资源共享");
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
            scheduler_.begin_world(generation);
            runtimeError_.clear();
        }
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto on_world_begin(const std::uint64_t generation) -> void {
        sessions_.reset();
        scheduler_.reset();
        restore_or_disable("切换世界");
        unregister_resource_hooks();
        worldDisabledErrors_ = {};
        catalog_ = {};
        baseCount_ = 0;
        containerCount_ = 0;
        worldContextFullName_.clear();
        runtime_.begin_world_transition(generation);
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto on_world_ready(const std::uint64_t generation) -> void {
        runtime_.finish_world_transition(generation);
        sessions_.begin_world(generation);
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
        if (sessionTransition.unionBecameIdle) {
            restore_or_disable("制作会话空闲");
        }

        if (scheduler_.advance(deltaSeconds, generation, !liveUnion_.active)) {
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
        sessions_.reset();
        scheduler_.reset();
        restore_or_disable("卸载 mod");
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

        catalog_ = std::move(next);
        baseCount_ = catalog_.plan.baseCount;
        containerCount_ = catalog_.plan.ordered.size();
        runtimeError_.clear();

        bool unionReady = true;
        const auto targets = sessions_.required_targets(generation);
        if (targets != UnionTargets{}) {
            unionReady = apply_live_union(worldContext, targets);
            if (!unionReady) {
                static_cast<void>(sessions_.cancel_all(generation));
            }
        }

        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: resource catalog reconciled in {:.3f} ms, bases={}, "
                "containers={}, union={}\n"),
            elapsed, baseCount_, containerCount_, liveUnion_.active);
        if (!unionReady) {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: resource union initialization failed after catalog "
                    "reconciliation; the current material operation was cancelled\n"));
        }
        snapshotDirty_.mark();
    }

    [[nodiscard]] auto apply_live_union(UObject* worldContext, const UnionTargets targets) -> bool {
        UnionTargets requestedTargets = targets;
        UnionTargets previousTargets;
        if (liveUnion_.active) {
            if (liveUnion_.generation != runtime_.generation() ||
                liveUnion_.guildId != catalog_.guildId) {
                return false;
            }
            if (contains_union_targets(liveUnion_.targets, targets)) {
                return true;
            }
            previousTargets = liveUnion_.targets;
            requestedTargets = combine_union_targets(previousTargets, targets);
            if (!restore_live_union("扩展材料操作联合")) {
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
            applied =
                detail::apply_union(worldContext, catalog_, requestedTargets, liveUnion_, error);
        }
        if (!applied) {
            if (previousTargets != UnionTargets{}) {
                std::string rollbackError;
                MutationScope mutation{selfMutation_};
                static_cast<void>(detail::apply_union(worldContext, catalog_, previousTargets,
                                                      liveUnion_, rollbackError));
            }
            runtimeError_ = std::move(error);
            snapshotDirty_.mark();
            return false;
        }

        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: resource union opened in {:.3f} ms, entries={}, modules={}, "
                "helper={}\n"),
            elapsed, liveUnion_.entries.size(), requestedTargets.baseModules,
            requestedTargets.playerHelper);
        runtimeError_.clear();
        snapshotDirty_.mark();
        return true;
    }

    [[nodiscard]] auto ensure_union(UObject* hint, const ResourceOperation operation) -> bool {
        const auto generation = runtime_.generation();
        if (!runtime_.can_extend(operation, generation)) {
            return false;
        }
        remember_world_context(hint);
        const auto targets = sessions_.required_targets(generation);
        if (hint == nullptr || targets == UnionTargets{} || !catalog_ready()) {
            scheduler_.request_immediate(generation);
            return false;
        }
        return apply_live_union(hint, targets);
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
        if (transition.unionBecameIdle) {
            restore_or_disable("退出建造模式");
        }
    }

    auto handle_acquire(UObject* context, const ResourceOperation operation) -> void {
        const auto generation = runtime_.generation();
        static_cast<void>(sessions_.acquire(operation, generation));
        remember_world_context(context);
        if (!catalog_ready()) {
            runtimeError_ = "正在初始化共享资源目录；目录就绪后会自动建立本次材料会话。";
            scheduler_.request_immediate(generation);
            snapshotDirty_.mark();
            return;
        }
        if (ensure_union(context, operation)) {
            return;
        }
        const auto transition = sessions_.release(operation, generation);
        if (transition.unionBecameIdle) {
            restore_or_disable("材料操作会话获取失败");
        }
        runtimeError_ = "资源目录尚未安全就绪；本次菜单保持原版行为，请重新打开后再试。";
        scheduler_.request_immediate(generation);
        snapshotDirty_.mark();
    }

    auto handle_touch(const ResourceOperation operation) -> void {
        static_cast<void>(sessions_.touch(operation, runtime_.generation()));
    }

    auto handle_release(const ResourceOperation operation, const std::string_view reason) -> void {
        const auto transition = sessions_.release(operation, runtime_.generation());
        if (transition.unionBecameIdle) {
            restore_or_disable(reason);
        }
    }

    auto handle_structure_changed() -> void {
        scheduler_.request_immediate(runtime_.generation());
    }

    auto dispatch_hook(const HookPhase phase, const HookSpec& spec,
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
                handle_acquire(context.Context, spec.operation);
                break;
            case HookEvent::touch:
                handle_touch(spec.operation);
                break;
            case HookEvent::release:
                handle_release(spec.operation, "制作请求已提交");
                break;
            case HookEvent::updateBuildingMode:
                update_building_mode(context.Context);
                break;
        }
        if (event != HookEvent::touch) {
            snapshotDirty_.mark();
        }
    }

    auto on_hook_pre(UFunction*, const HookSpec& spec, UnrealScriptFunctionCallableContext& context)
        -> void {
        dispatch_hook(HookPhase::pre, spec, context);
    }

    auto on_hook_post(UFunction*, const HookSpec& spec,
                      UnrealScriptFunctionCallableContext& context) -> void {
        dispatch_hook(HookPhase::post, spec, context);
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
    MaterialOperationSessions sessions_;
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
    bool selfMutation_{};
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
