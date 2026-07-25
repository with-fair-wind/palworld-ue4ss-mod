#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <base_resource_sharing/hook_manifest.hpp>
#include <base_resource_sharing/pal_base_resources.hpp>
#include <support/text_encoding.hpp>

namespace base_resource_sharing {
using namespace RC;
using namespace RC::Unreal;

namespace {
[[nodiscard]] auto to_key(const FGuid& guid) -> GuidKey {
    return {{guid.A, guid.B, guid.C, guid.D}};
}

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

[[nodiscard]] auto try_get_guid(UObject* target, const CharType* functionName, FGuid& output)
    -> bool {
    if (target == nullptr) {
        return false;
    }
    auto* function = target->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return false;
    }
    struct Params {
        FGuid ReturnValue{};
    } params;
    target->ProcessEvent(function, &params);
    output = params.ReturnValue;
    return to_key(output).valid();
}

[[nodiscard]] auto try_get_object(UObject* target, const CharType* functionName, UObject*& output)
    -> bool {
    output = nullptr;
    if (target == nullptr) {
        return false;
    }
    auto* function = target->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return false;
    }
    struct Params {
        UObject* ReturnValue{};
    } params;
    target->ProcessEvent(function, &params);
    output = params.ReturnValue;
    return output != nullptr;
}

[[nodiscard]] auto try_get_player_guild(UObject* worldContext, const FGuid& playerId,
                                        UObject*& guild) -> bool {
    guild = nullptr;
    auto* utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* function = utility == nullptr
                         ? nullptr
                         : utility->GetFunctionByNameInChain(STR("GetGuildByPlayerUId"));
    if (function == nullptr) {
        return false;
    }
    struct Params {
        UObject* WorldContextObject{};
        FGuid PlayerUId{};
        UObject* ReturnValue{};
    } params{.WorldContextObject = worldContext, .PlayerUId = playerId};
    utility->ProcessEvent(function, &params);
    guild = params.ReturnValue;
    return guild != nullptr;
}

[[nodiscard]] auto try_resolve_local_guild(UObject* worldContext, FGuid& guildId) -> bool {
    auto* utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* function = utility == nullptr
                         ? nullptr
                         : utility->GetFunctionByNameInChain(STR("GetLocalPalPlayerController"));
    if (function == nullptr) {
        return false;
    }
    struct Params {
        UObject* WorldContextObject{};
        UObject* ReturnValue{};
    } params{.WorldContextObject = worldContext};
    utility->ProcessEvent(function, &params);

    FGuid playerId{};
    UObject* guild{};
    return params.ReturnValue != nullptr &&
           try_get_guid(params.ReturnValue, STR("GetPlayerUId"), playerId) &&
           try_get_player_guild(params.ReturnValue, playerId, guild) &&
           try_get_guid(guild, STR("GetId"), guildId);
}

[[nodiscard]] auto try_resolve_request_guild(UObject* requestComponent, FGuid& guildId) -> bool {
    UObject* transmitter{};
    UObject* controller{};
    FGuid playerId{};
    UObject* guild{};
    return try_get_object(requestComponent, STR("GetOwner"), transmitter) &&
           try_get_object(transmitter, STR("GetOwner"), controller) &&
           try_get_guid(controller, STR("GetPlayerUId"), playerId) &&
           try_get_player_guild(controller, playerId, guild) &&
           try_get_guid(guild, STR("GetId"), guildId);
}

[[nodiscard]] auto try_resolve_preview_guild(UObject* context, FGuid& guildId) -> bool {
    UObject* owner{};
    UObject* controller{};
    if (try_get_object(context, STR("GetOwner"), owner)) {
        static_cast<void>(try_get_object(owner, STR("GetPalPlayerController"), controller));
    }
    if (controller != nullptr) {
        FGuid playerId{};
        UObject* guild{};
        if (try_get_guid(controller, STR("GetPlayerUId"), playerId) &&
            try_get_player_guild(controller, playerId, guild) &&
            try_get_guid(guild, STR("GetId"), guildId)) {
            return true;
        }
    }
    if (try_resolve_local_guild(context, guildId)) {
        return true;
    }
    auto* worldContext = UObjectGlobals::FindFirstOf(STR("PalPlayerInventoryData"));
    return worldContext != nullptr && worldContext != context &&
           try_resolve_local_guild(worldContext, guildId);
}

[[nodiscard]] auto try_get_container_guid(FArrayProperty* arrayProperty, void* containerInfo,
                                          FGuid& output) -> bool {
    auto* infoProperty =
        arrayProperty == nullptr ? nullptr : CastField<FStructProperty>(arrayProperty->GetInner());
    auto* infoStruct = infoProperty == nullptr ? nullptr : infoProperty->GetStruct().Get();
    auto* containerIdProperty =
        infoStruct == nullptr ? nullptr
                              : CastField<FStructProperty>(
                                    infoStruct->GetPropertyByNameInChain(STR("ContainerIdCache")));
    auto* containerIdStruct =
        containerIdProperty == nullptr ? nullptr : containerIdProperty->GetStruct().Get();
    auto* idProperty =
        containerIdStruct == nullptr
            ? nullptr
            : CastField<FStructProperty>(containerIdStruct->GetPropertyByNameInChain(STR("ID")));
    if (containerInfo == nullptr || containerIdProperty == nullptr || idProperty == nullptr) {
        return false;
    }
    auto* containerId = containerIdProperty->ContainerPtrToValuePtr<void>(containerInfo);
    auto* idAddress = idProperty->ContainerPtrToValuePtr<void>(containerId);
    idProperty->CopyCompleteValue(&output, idAddress);
    return to_key(output).valid();
}

[[nodiscard]] auto try_get_item_container_guid(UObject* container, FGuid& output) -> bool {
    return try_get_guid(container, STR("GetId"), output);
}

[[nodiscard]] auto append_array_copy(FArrayProperty* arrayProperty, UObject* target,
                                     const void* source) -> bool {
    if (arrayProperty == nullptr || target == nullptr || source == nullptr) {
        return false;
    }
    auto* inner = arrayProperty->GetInner();
    auto* array = arrayProperty->ContainerPtrToValuePtr<FScriptArray>(target);
    if (inner == nullptr || array == nullptr) {
        return false;
    }
    const auto elementSize = inner->GetElementSize();
    const auto alignment = inner->GetMinAlignment();
    const auto index = array->Add(1, elementSize, alignment);
    auto* destination = static_cast<std::uint8_t*>(array->GetData()) +
                        static_cast<std::size_t>(index) * elementSize;
    inner->InitializeValue(destination);
    inner->CopyCompleteValue(destination, source);
    return true;
}

[[nodiscard]] auto remove_array_tail(FArrayProperty* arrayProperty, UObject* target,
                                     const std::size_t originalSize) -> bool {
    if (arrayProperty == nullptr || target == nullptr) {
        return false;
    }
    auto* inner = arrayProperty->GetInner();
    auto* array = arrayProperty->ContainerPtrToValuePtr<FScriptArray>(target);
    if (inner == nullptr || array == nullptr || array->Num() < 0 ||
        static_cast<std::size_t>(array->Num()) < originalSize) {
        return false;
    }
    const auto elementSize = inner->GetElementSize();
    const auto alignment = inner->GetMinAlignment();
    const auto originalCount = static_cast<int32>(originalSize);
    const auto removeCount = array->Num() - originalCount;
    for (auto index = originalCount; index < array->Num(); ++index) {
        auto* element = static_cast<std::uint8_t*>(array->GetData()) +
                        static_cast<std::size_t>(index) * elementSize;
        inner->DestroyValue(element);
    }
    if (removeCount > 0) {
        array->Remove(originalCount, removeCount, elementSize, alignment);
    }
    return array->Num() == originalCount;
}

[[nodiscard]] auto steady_seconds() -> double {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] auto resolve_main_inventory_container() -> UObject* {
    auto* inventory = UObjectGlobals::FindFirstOf(STR("PalPlayerInventoryData"));
    auto* function =
        inventory == nullptr
            ? nullptr
            : inventory->GetFunctionByNameInChain(STR("TryGetContainerFromInventoryType"));
    if (function == nullptr) {
        return nullptr;
    }
    struct Params {
        std::uint8_t Type{};
        UObject* Out{};
        bool ReturnValue{};
    } params;
    inventory->ProcessEvent(function, &params);
    return params.Out;
}

[[nodiscard]] auto read_container_amounts(UObject* container, std::vector<ItemAmount>& output,
                                          std::string& error) -> bool {
    auto* arrayProperty =
        container == nullptr
            ? nullptr
            : CastField<FArrayProperty>(container->GetPropertyByNameInChain(STR("ItemSlotArray")));
    auto* objectProperty = arrayProperty == nullptr
                               ? nullptr
                               : CastField<FObjectPropertyBase>(arrayProperty->GetInner());
    if (arrayProperty == nullptr || objectProperty == nullptr) {
        error = "物品容器缺少 ItemSlotArray 对象数组。";
        return false;
    }

    FScriptArrayHelper_InContainer slots(arrayProperty, container);
    for (int32 index{}; index < slots.Num(); ++index) {
        auto* slot = objectProperty->GetObjectPropertyValue(slots.GetRawPtr(index));
        if (slot == nullptr) {
            continue;
        }
        auto* itemIdProperty =
            CastField<FStructProperty>(slot->GetPropertyByNameInChain(STR("ItemId")));
        auto* itemIdStruct =
            itemIdProperty == nullptr ? nullptr : itemIdProperty->GetStruct().Get();
        auto* staticIdProperty =
            itemIdStruct == nullptr
                ? nullptr
                : CastField<FNameProperty>(itemIdStruct->GetPropertyByNameInChain(STR("StaticId")));
        auto* stackProperty =
            CastField<FIntProperty>(slot->GetPropertyByNameInChain(STR("StackCount")));
        if (itemIdProperty == nullptr || staticIdProperty == nullptr || stackProperty == nullptr) {
            error = "物品槽缺少 ItemId.StaticId 或 StackCount。";
            return false;
        }

        auto* itemId = itemIdProperty->ContainerPtrToValuePtr<void>(slot);
        auto* staticIdAddress = staticIdProperty->ContainerPtrToValuePtr<void>(itemId);
        const auto& staticId = staticIdProperty->GetPropertyValue(staticIdAddress);
        const auto count = stackProperty->GetPropertyValueInContainer(slot);
        if (count < 0) {
            error = "物品槽数量为负数。";
            return false;
        }
        if (count == 0) {
            continue;
        }
        auto id = text_encoding::to_utf8(staticId.ToString());
        if (id.empty() || id == "None") {
            error = "非空物品槽缺少有效 StaticId。";
            return false;
        }
        output.push_back({.id = std::move(id), .amount = count});
    }
    return true;
}
}  // namespace

class PalBaseResourceBridge::Impl {
public:
    struct Module {
        UObject* object{};
        FArrayProperty* property{};
        GuidKey baseId;
    };

    struct Source {
        UObject* module{};
        FArrayProperty* property{};
        int32 index{};
        GuidKey containerId;
    };

    struct Discovery {
        GuidKey guildId;
        ResourceUnionPlan plan;
        std::vector<Module> modules;
        std::vector<Source> sources;
        std::map<GuidKey, UObject*> liveContainers;
        std::string error;
    };

    struct RuntimePatch {
        ArrayPatchLedger ledger;
    };

    struct HookBinding {
        UFunction* function{};
        HookSpec spec;
        std::pair<int, int> ids{-1, -1};
    };

    struct PreviewCountSnapshot {
        std::uint64_t generation{};
        std::map<std::string, std::int64_t> amounts;
        std::string error;
    };

    struct RequirementsResult {
        std::vector<ItemAmount> requirements;
        std::string error;
    };

    auto set_enabled(const bool enabled) -> void {
        if (runtime_.enabled() == enabled) {
            return;
        }
        bool restored = true;
        if (!enabled && unionActive_) {
            restored = restore_union();
        }
        runtime_.set_preference(enabled);
        if (!enabled) {
            requestGuard_.reset();
            buildWindow_.reset();
            synchronousOwner_ = nullptr;
            synchronousDepth_ = 0;
            if (restored) {
                runtimeError_.clear();
            } else {
                constexpr std::string_view error =
                    "关闭共享时未能验证完整恢复；重新进入世界前制作和建造共享均保持禁用。";
                disable_operation(ResourceOperation::crafting, std::string{error});
                disable_operation(ResourceOperation::building, std::string{error});
            }
            unregister_resource_hooks();
        }
        previewCacheGate_.invalidate();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto set_config_error(std::string error) -> void {
        const std::lock_guard lock(snapshotMutex_);
        snapshot_.configError = std::move(error);
    }

    auto on_world_begin(const std::uint64_t generation) -> void {
        if (unionActive_ && !restore_union()) {
            runtimeError_ = "世界切换前恢复跨据点资源联合失败；共享已关闭。";
        }
        requestGuard_.reset();
        buildWindow_.reset();
        synchronousOwner_ = nullptr;
        synchronousDepth_ = 0;
        unregister_resource_hooks();
        worldDisabledErrors_ = {};
        restorationFailed_ = false;
        runtime_.begin_world_transition(generation);
        baseCount_ = 0;
        containerCount_ = 0;
        previewCacheGate_.invalidate();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto on_world_ready(const std::uint64_t generation) -> void {
        runtime_.finish_world_transition(generation);
        publish_capabilities();
        previewCacheGate_.invalidate();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    auto tick(const float deltaSeconds) -> void {
        if (buildWindow_.advance(deltaSeconds, runtime_.generation())) {
            const bool restored = restore_union();
            requestGuard_.leave(ResourceOperation::building, runtime_.generation());
            if (!restored) {
                disable_operation(ResourceOperation::building,
                                  "建造资源联合恢复验证失败；本世界已禁用建造共享。");
            }
            snapshotDirty_.mark();
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
        if (hooks_.size() == palworld_1_0_1_hook_manifest().size() &&
            capabilitiesGeneration_ == runtime_.generation()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < nextHookAttempt_) {
            if (capabilitiesGeneration_ != runtime_.generation()) {
                publish_capabilities();
                publish_snapshot();
            }
            return;
        }
        nextHookAttempt_ = now + std::chrono::seconds{1};

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
        unregister_resource_hooks();
        runtime_.begin_world_transition(runtime_.generation() + 1);
        previewCacheGate_.invalidate();
        snapshotDirty_.mark();
        publish_snapshot();
    }

    [[nodiscard]] auto snapshot() const -> BaseResourceSharingSnapshot {
        const std::lock_guard lock(snapshotMutex_);
        return snapshot_;
    }

private:
    auto unregister_resource_hooks() -> void {
        if (unionActive_) {
            static_cast<void>(restore_union());
        }
        for (auto hook = hooks_.rbegin(); hook != hooks_.rend(); ++hook) {
            if (hook->function != nullptr && hook->ids.first >= 0) {
                UObjectGlobals::UnregisterHook(hook->function, hook->ids);
            }
        }
        hooks_.clear();
        resolutions_ = all_hook_resolutions(false);
        capabilitiesGeneration_ = 0;
        nextHookAttempt_ = {};
        requestGuard_.reset();
        buildWindow_.reset();
        synchronousOwner_ = nullptr;
        synchronousDepth_ = 0;
        previewCacheGate_.invalidate();
        publish_capabilities();
        snapshotDirty_.mark();
    }

    [[nodiscard]] auto discover_guild(const GuidKey& guildId, Discovery& output) -> bool {
        output = {};
        output.guildId = guildId;
        auto* baseClass = UObjectGlobals::StaticFindObject<UClass*>(
            nullptr, nullptr, STR("/Script/Pal.PalBaseCampModel"));
        if (baseClass == nullptr || !guildId.valid()) {
            output.error = "无法解析据点模型或当前公会。";
            return false;
        }

        std::vector<UObject*> allModules;
        UObjectGlobals::FindAllOf(STR("PalBaseCampModuleItemStorage"), allModules);
        std::vector<ContainerDescriptor> descriptors;
        std::set<GuidKey> sourceIds;
        for (auto* module : allModules) {
            auto* base = module == nullptr ? nullptr : module->GetTypedOuter(baseClass);
            FGuid ownerGuild{};
            FGuid baseId{};
            auto* property = module == nullptr
                                 ? nullptr
                                 : CastField<FArrayProperty>(
                                       module->GetPropertyByNameInChain(STR("ContainerInfos")));
            if (property == nullptr || !try_get_guid(base, STR("GetGroupIdBelongTo"), ownerGuild) ||
                to_key(ownerGuild) != guildId) {
                continue;
            }
            if (!try_get_guid(base, STR("GetId"), baseId)) {
                output.error = "同公会据点缺少有效据点标识。";
                return false;
            }

            output.modules.push_back(
                {.object = module, .property = property, .baseId = to_key(baseId)});
            FScriptArrayHelper_InContainer infos(property, module);
            for (int32 index{}; index < infos.Num(); ++index) {
                FGuid containerId{};
                if (!try_get_container_guid(property, infos.GetRawPtr(index), containerId)) {
                    output.error = "据点普通仓储登记项缺少有效容器标识。";
                    return false;
                }
                const auto key = to_key(containerId);
                descriptors.push_back({.baseId = to_key(baseId),
                                       .groupId = guildId,
                                       .containerId = key,
                                       .kind = ContainerKind::normal});
                if (sourceIds.insert(key).second) {
                    output.sources.push_back({.module = module,
                                              .property = property,
                                              .index = index,
                                              .containerId = key});
                }
            }
        }

        output.plan = make_resource_union_plan(descriptors, guildId);
        if (!output.plan.error.empty()) {
            output.error = output.plan.error;
            return false;
        }

        std::vector<UObject*> allContainers;
        UObjectGlobals::FindAllOf(STR("PalItemContainer"), allContainers);
        std::vector<GuidKey> resolvedIds;
        for (auto* container : allContainers) {
            FGuid id{};
            if (!try_get_item_container_guid(container, id)) {
                continue;
            }
            const auto key = to_key(id);
            if (sourceIds.contains(key) && !output.liveContainers.contains(key)) {
                output.liveContainers.emplace(key, container);
                resolvedIds.push_back(key);
            }
        }
        const auto validation =
            validate_live_container_resolution(output.plan.ordered, resolvedIds);
        if (!validation.error.empty()) {
            output.error = validation.error;
            return false;
        }
        return true;
    }

    [[nodiscard]] static auto read_module_sequence(UObject* module, FArrayProperty* property,
                                                   std::vector<GuidKey>& ids) -> bool {
        ids.clear();
        if (module == nullptr || property == nullptr) {
            return false;
        }
        FScriptArrayHelper_InContainer infos(property, module);
        ids.reserve(static_cast<std::size_t>(std::max(infos.Num(), 0)));
        for (int32 index{}; index < infos.Num(); ++index) {
            FGuid id{};
            if (!try_get_container_guid(property, infos.GetRawPtr(index), id)) {
                return false;
            }
            ids.push_back(to_key(id));
        }
        return true;
    }

    [[nodiscard]] static auto read_helper_sequence(UObject* helper, FArrayProperty* property,
                                                   std::vector<GuidKey>& ids) -> bool {
        ids.clear();
        auto* objectProperty =
            property == nullptr ? nullptr : CastField<FObjectPropertyBase>(property->GetInner());
        if (helper == nullptr || property == nullptr || objectProperty == nullptr) {
            return false;
        }
        FScriptArrayHelper_InContainer containers(property, helper);
        ids.reserve(static_cast<std::size_t>(std::max(containers.Num(), 0)));
        for (int32 index{}; index < containers.Num(); ++index) {
            auto* container = objectProperty->GetObjectPropertyValue(containers.GetRawPtr(index));
            FGuid id{};
            if (!try_get_item_container_guid(container, id)) {
                return false;
            }
            ids.push_back(to_key(id));
        }
        return true;
    }

    [[nodiscard]] auto refresh_preview_counts(UObject* worldContext) -> bool {
        const auto nowSeconds = steady_seconds();
        if (previewCacheGate_.can_reuse(runtime_.generation(), nowSeconds)) {
            return previewCounts_.error.empty();
        }

        const auto started = std::chrono::steady_clock::now();
        PreviewCountSnapshot next{.generation = runtime_.generation()};
        std::vector<ItemAmount> playerAmounts;
        std::vector<ItemAmount> baseAmounts;
        FGuid guild{};
        Discovery discovery;

        if (worldContext == nullptr) {
            worldContext = UObjectGlobals::FindFirstOf(STR("PalPlayerInventoryData"));
        }
        if (worldContext == nullptr || !try_resolve_local_guild(worldContext, guild)) {
            next.error = "预览缓存无法解析本地玩家公会。";
        } else if (!discover_guild(to_key(guild), discovery)) {
            next.error = discovery.error;
        } else {
            auto* playerContainer = resolve_main_inventory_container();
            if (!read_container_amounts(playerContainer, playerAmounts, next.error)) {
                if (next.error.empty()) {
                    next.error = "预览缓存无法读取玩家 Common 背包。";
                }
            } else {
                for (const auto& [id, container] : discovery.liveContainers) {
                    static_cast<void>(id);
                    if (!read_container_amounts(container, baseAmounts, next.error)) {
                        break;
                    }
                }
            }
        }

        if (next.error.empty()) {
            auto combined = combine_preview_sources(playerAmounts, baseAmounts);
            next.amounts = std::move(combined.amounts);
            next.error = std::move(combined.error);
        }

        if (next.error.empty()) {
            baseCount_ = discovery.plan.baseCount;
            containerCount_ = discovery.plan.ordered.size();
            if (std::ranges::all_of(worldDisabledErrors_,
                                    [](const auto& error) { return error.empty(); })) {
                runtimeError_.clear();
            }
        } else {
            runtimeError_ = next.error;
        }
        previewCounts_ = std::move(next);
        previewCacheGate_.record(runtime_.generation(), nowSeconds);
        snapshotDirty_.mark();

        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: resource preview cache rebuilt in {:.3f} ms, items={}, "
                "bases={}, containers={}, success={}\n"),
            elapsed, previewCounts_.amounts.size(), baseCount_, containerCount_,
            previewCounts_.error.empty());
        return previewCounts_.error.empty();
    }

    [[nodiscard]] static auto read_crafting_requirements(UObject* model) -> RequirementsResult {
        RequirementsResult result;
        auto* function = model == nullptr
                             ? nullptr
                             : model->GetFunctionByNameInChain(STR("GetRequiredMaterialInfos"));
        auto* arrayProperty = function == nullptr
                                  ? nullptr
                                  : CastField<FArrayProperty>(function->FindProperty(
                                        FName(STR("RequiredMaterialInfos"), FNAME_Find)));
        auto* oneUnitProperty =
            function == nullptr ? nullptr
                                : CastField<FBoolProperty>(
                                      function->FindProperty(FName(STR("OneUnit"), FNAME_Find)));
        auto* innerStructProperty = arrayProperty == nullptr
                                        ? nullptr
                                        : CastField<FStructProperty>(arrayProperty->GetInner());
        auto* innerStruct =
            innerStructProperty == nullptr ? nullptr : innerStructProperty->GetStruct().Get();
        auto* idProperty = innerStruct == nullptr
                               ? nullptr
                               : CastField<FNameProperty>(
                                     innerStruct->GetPropertyByNameInChain(STR("StaticItemId")));
        auto* countProperty =
            innerStruct == nullptr
                ? nullptr
                : CastField<FIntProperty>(innerStruct->GetPropertyByNameInChain(STR("Num")));
        if (function == nullptr || arrayProperty == nullptr || oneUnitProperty == nullptr ||
            innerStructProperty == nullptr || idProperty == nullptr || countProperty == nullptr) {
            result.error = "制作材料预览参数布局不兼容。";
            return result;
        }

        std::vector<std::byte> params(function->GetParmsSize());
        arrayProperty->InitializeValue_InContainer(params.data());
        struct ArrayGuard {
            FArrayProperty* property{};
            void* container{};
            ~ArrayGuard() {
                property->DestroyValue_InContainer(container);
            }
        } guard{.property = arrayProperty, .container = params.data()};
        oneUnitProperty->SetPropertyValueInContainer(params.data(), true);
        model->ProcessEvent(function, params.data());

        FScriptArrayHelper_InContainer values(arrayProperty, params.data());
        result.requirements.reserve(static_cast<std::size_t>(std::max(values.Num(), 0)));
        for (int32 index{}; index < values.Num(); ++index) {
            auto* element = values.GetRawPtr(index);
            auto* idAddress = idProperty->ContainerPtrToValuePtr<void>(element);
            const auto& id = idProperty->GetPropertyValue(idAddress);
            const auto count = countProperty->GetPropertyValueInContainer(element);
            auto rawId = text_encoding::to_utf8(id.ToString());
            if (rawId.empty() || rawId == "None" || count < 0) {
                result.requirements.clear();
                result.error = "制作材料预览包含无效物品 ID 或数量。";
                return result;
            }
            if (count > 0) {
                result.requirements.push_back({.id = std::move(rawId), .amount = count});
            }
        }
        return result;
    }

    [[nodiscard]] static auto read_building_requirements(
        UFunction* function, UnrealScriptFunctionCallableContext& context) -> RequirementsResult {
        RequirementsResult result;
        auto* buildDataProperty = function == nullptr
                                      ? nullptr
                                      : CastField<FStructProperty>(function->FindProperty(
                                            FName(STR("BuildObjectData"), FNAME_Find)));
        auto* buildDataStruct =
            buildDataProperty == nullptr ? nullptr : buildDataProperty->GetStruct().Get();
        auto* locals = context.TheStack.Locals();
        if (buildDataProperty == nullptr || buildDataStruct == nullptr || locals == nullptr) {
            result.error = "建造材料预览参数布局不兼容。";
            return result;
        }
        auto* buildData = buildDataProperty->ContainerPtrToValuePtr<void>(locals);
        constexpr std::array<const CharType*, 4> idNames{STR("Material1_Id"), STR("Material2_Id"),
                                                         STR("Material3_Id"), STR("Material4_Id")};
        constexpr std::array<const CharType*, 4> countNames{
            STR("Material1_Count"), STR("Material2_Count"), STR("Material3_Count"),
            STR("Material4_Count")};
        for (std::size_t index{}; index < idNames.size(); ++index) {
            auto* idProperty =
                CastField<FNameProperty>(buildDataStruct->GetPropertyByNameInChain(idNames[index]));
            auto* countProperty = CastField<FIntProperty>(
                buildDataStruct->GetPropertyByNameInChain(countNames[index]));
            if (idProperty == nullptr || countProperty == nullptr) {
                result.requirements.clear();
                result.error = "建造材料字段布局不兼容。";
                return result;
            }
            auto* idAddress = idProperty->ContainerPtrToValuePtr<void>(buildData);
            const auto& id = idProperty->GetPropertyValue(idAddress);
            const auto count = countProperty->GetPropertyValueInContainer(buildData);
            if (count < 0) {
                result.requirements.clear();
                result.error = "建造材料数量为负数。";
                return result;
            }
            if (count == 0) {
                continue;
            }
            auto rawId = text_encoding::to_utf8(id.ToString());
            if (rawId.empty() || rawId == "None") {
                result.requirements.clear();
                result.error = "建造材料包含无效物品 ID。";
                return result;
            }
            result.requirements.push_back({.id = std::move(rawId), .amount = count});
        }
        if (result.requirements.empty()) {
            result.error = "建造对象没有可验证的材料需求。";
        }
        return result;
    }

    auto update_preview_result(UFunction* function, const HookSpec& spec,
                               UnrealScriptFunctionCallableContext& context) -> void {
        if (!runtime_.can_extend(spec.operation, runtime_.generation()) ||
            context.RESULT_DECL == nullptr) {
            return;
        }
        auto* worldContext = UObjectGlobals::FindFirstOf(STR("PalPlayerInventoryData"));
        if (!refresh_preview_counts(worldContext)) {
            snapshotDirty_.mark();
            publish_snapshot();
            return;
        }

        if (spec.operation == ResourceOperation::crafting) {
            auto* returnProperty = function == nullptr
                                       ? nullptr
                                       : CastField<FIntProperty>(function->GetReturnProperty());
            const auto requirements = read_crafting_requirements(context.Context);
            if (returnProperty == nullptr || !requirements.error.empty()) {
                if (!requirements.error.empty()) {
                    runtimeError_ = requirements.error;
                    snapshotDirty_.mark();
                }
                publish_snapshot();
                return;
            }
            const auto vanilla = returnProperty->GetPropertyValue(context.RESULT_DECL);
            const auto shared = max_productable_from_shared_counts(
                vanilla, requirements.requirements, previewCounts_.amounts);
            returnProperty->SetPropertyValue(context.RESULT_DECL, shared);
        } else if (spec.operation == ResourceOperation::building) {
            auto* returnProperty = function == nullptr
                                       ? nullptr
                                       : CastField<FBoolProperty>(function->GetReturnProperty());
            if (returnProperty == nullptr ||
                returnProperty->GetPropertyValue(context.RESULT_DECL)) {
                publish_snapshot();
                return;
            }
            const auto requirements = read_building_requirements(function, context);
            if (!requirements.error.empty()) {
                runtimeError_ = requirements.error;
                snapshotDirty_.mark();
                publish_snapshot();
                return;
            }
            if (shared_requirements_available(requirements.requirements, previewCounts_.amounts)) {
                returnProperty->SetPropertyValue(context.RESULT_DECL, true);
            }
        }
        publish_snapshot();
    }

    [[nodiscard]] auto begin_union(const GuidKey& guildId) -> bool {
        const auto started = std::chrono::steady_clock::now();
        if (unionActive_) {
            runtimeError_ = "已有跨据点资源请求正在执行。";
            return false;
        }

        Discovery discovery;
        if (!discover_guild(guildId, discovery)) {
            runtimeError_ = discovery.error;
            const auto elapsed = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            Output::send<LogLevel::Verbose>(
                STR("PalworldEditor: live resource union preparation failed in {:.3f} ms\n"),
                elapsed);
            return false;
        }

        unionActive_ = true;
        unionOpenedAt_ = started;
        activeGeneration_ = runtime_.generation();
        patches_.clear();
        std::vector<GuidKey> globalIds;
        globalIds.reserve(discovery.plan.ordered.size());
        for (const auto& descriptor : discovery.plan.ordered) {
            globalIds.push_back(descriptor.containerId);
        }

        for (const auto& module : discovery.modules) {
            RuntimePatch patch{
                .ledger = {.objectFullName = object_name(module.object), .helperArray = false},
            };
            if (!read_module_sequence(module.object, module.property, patch.ledger.original)) {
                static_cast<void>(restore_union());
                runtimeError_ = "读取据点资源容器序列失败。";
                return false;
            }
            const auto missing = missing_union_tail(patch.ledger.original, globalIds);
            patches_.push_back(patch);
            for (const auto& missingId : missing) {
                const auto source =
                    std::ranges::find(discovery.sources, missingId, &Source::containerId);
                if (source == discovery.sources.end()) {
                    static_cast<void>(restore_union());
                    runtimeError_ = "资源联合源容器在准备期间消失。";
                    return false;
                }
                FScriptArrayHelper_InContainer sourceInfos(source->property, source->module);
                if (source->index < 0 || source->index >= sourceInfos.Num() ||
                    !append_array_copy(module.property, module.object,
                                       sourceInfos.GetRawPtr(source->index))) {
                    static_cast<void>(restore_union());
                    runtimeError_ = "追加据点资源容器引用失败。";
                    return false;
                }
                patches_.back().ledger.appended.push_back(missingId);
            }
        }

        std::vector<UObject*> helpers;
        UObjectGlobals::FindAllOf(STR("PalItemContainerMultiHelper"), helpers);
        for (auto* helper : helpers) {
            auto* property = helper == nullptr
                                 ? nullptr
                                 : CastField<FArrayProperty>(
                                       helper->GetPropertyByNameInChain(STR("Containers")));
            auto* objectProperty = property == nullptr
                                       ? nullptr
                                       : CastField<FObjectPropertyBase>(property->GetInner());
            if (property == nullptr || objectProperty == nullptr) {
                continue;
            }

            RuntimePatch patch{
                .ledger = {.objectFullName = object_name(helper), .helperArray = true},
            };
            if (!read_helper_sequence(helper, property, patch.ledger.original)) {
                continue;
            }
            const auto missing = missing_union_tail(patch.ledger.original, globalIds);
            if (missing.empty()) {
                continue;
            }
            patches_.push_back(patch);
            for (const auto& missingId : missing) {
                const auto live = discovery.liveContainers.find(missingId);
                if (live == discovery.liveContainers.end()) {
                    static_cast<void>(restore_union());
                    runtimeError_ = "追加资源助手时容器对象已失效。";
                    return false;
                }
                auto* value = live->second;
                if (!append_array_copy(property, helper, &value)) {
                    static_cast<void>(restore_union());
                    runtimeError_ = "追加资源助手容器引用失败。";
                    return false;
                }
                patches_.back().ledger.appended.push_back(missingId);
            }
        }

        baseCount_ = discovery.plan.baseCount;
        containerCount_ = discovery.plan.ordered.size();
        snapshotDirty_.mark();
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: live resource union prepared in {:.3f} ms, bases={}, "
                "containers={}, patches={}\n"),
            elapsed, baseCount_, containerCount_, patches_.size());
        return true;
    }

    [[nodiscard]] auto restore_union() -> bool {
        if (!unionActive_) {
            return true;
        }
        const auto restoreStarted = std::chrono::steady_clock::now();
        bool restored = activeGeneration_ == runtime_.generation();
        for (auto patch = patches_.rbegin(); patch != patches_.rend(); ++patch) {
            auto* object = find_object_by_full_name(patch->ledger.objectFullName);
            if (object == nullptr) {
                if (!patch->ledger.helperArray) {
                    restored = false;
                }
                continue;
            }
            const auto* propertyName =
                patch->ledger.helperArray ? STR("Containers") : STR("ContainerInfos");
            auto* property =
                CastField<FArrayProperty>(object->GetPropertyByNameInChain(propertyName));
            if (property == nullptr) {
                if (!patch->ledger.helperArray) {
                    restored = false;
                }
                continue;
            }
            std::vector<GuidKey> current;
            const bool read = patch->ledger.helperArray
                                  ? read_helper_sequence(object, property, current)
                                  : read_module_sequence(object, property, current);
            if (!read ||
                !verify_restoration_sequence(patch->ledger.original, current,
                                             patch->ledger.appended) ||
                !remove_array_tail(property, object, patch->ledger.original.size())) {
                restored = false;
                continue;
            }
            std::vector<GuidKey> finalSequence;
            const bool finalRead = patch->ledger.helperArray
                                       ? read_helper_sequence(object, property, finalSequence)
                                       : read_module_sequence(object, property, finalSequence);
            restored =
                finalRead && std::ranges::equal(finalSequence, patch->ledger.original) && restored;
        }
        patches_.clear();
        unionActive_ = false;
        activeGeneration_ = 0;
        restorationFailed_ = !restored;
        const auto restoredAt = std::chrono::steady_clock::now();
        const auto restoreElapsed =
            std::chrono::duration<double, std::milli>(restoredAt - restoreStarted).count();
        const auto activeElapsed =
            unionOpenedAt_ == std::chrono::steady_clock::time_point{}
                ? 0.0
                : std::chrono::duration<double, std::milli>(restoredAt - unionOpenedAt_).count();
        unionOpenedAt_ = {};
        Output::send<LogLevel::Verbose>(
            restored ? STR("PalworldEditor: live resource union restored in {:.3f} ms "
                           "(active {:.3f} ms)\n")
                     : STR("PalworldEditor: live resource union restoration failed in {:.3f} ms "
                           "(active {:.3f} ms)\n"),
            restoreElapsed, activeElapsed);
        return restored;
    }

    auto on_hook_pre(UFunction* function, const HookSpec& spec,
                     UnrealScriptFunctionCallableContext& context) -> void {
        if (!runtime_.can_extend(spec.operation, runtime_.generation())) {
            return;
        }
        if (spec.role == HookRole::preview) {
            return;
        }
        if (unionActive_) {
            if (synchronousOwner_ == function) {
                ++synchronousDepth_;
            }
            return;
        }
        if (!requestGuard_.try_enter(spec.operation, runtime_.generation())) {
            return;
        }
        previewCacheGate_.invalidate();

        FGuid guild{};
        const bool authorityRequest = spec.operation == ResourceOperation::building;
        const bool guildResolved = authorityRequest
                                       ? try_resolve_request_guild(context.Context, guild)
                                       : try_resolve_preview_guild(context.Context, guild);
        if (!guildResolved) {
            requestGuard_.leave(spec.operation, runtime_.generation());
            publish_snapshot();
            return;
        }
        if (!begin_union(to_key(guild))) {
            if (restorationFailed_) {
                disable_operation(spec.operation,
                                  "资源联合准备失败且未能验证完整恢复；本世界已禁用该类共享。");
            }
            snapshotDirty_.mark();
            requestGuard_.leave(spec.operation, runtime_.generation());
            publish_snapshot();
            return;
        }

        if (authorityRequest) {
            if (!buildWindow_.open(runtime_.generation())) {
                static_cast<void>(restore_union());
                requestGuard_.leave(spec.operation, runtime_.generation());
            }
        } else {
            synchronousOwner_ = function;
            synchronousOperation_ = spec.operation;
            synchronousDepth_ = 1;
        }
    }

    auto on_hook_post(UFunction* function, const HookSpec& spec,
                      UnrealScriptFunctionCallableContext& context) -> void {
        if (spec.role == HookRole::preview) {
            update_preview_result(function, spec, context);
            return;
        }
        if (synchronousOwner_ != function || synchronousDepth_ == 0) {
            return;
        }
        --synchronousDepth_;
        if (synchronousDepth_ != 0) {
            return;
        }
        const auto operation = synchronousOperation_;
        synchronousOwner_ = nullptr;
        const bool restored = restore_union();
        requestGuard_.leave(operation, runtime_.generation());
        if (!restored) {
            disable_operation(operation, "同步资源联合恢复验证失败；本世界已禁用该类共享。");
        }
        previewCacheGate_.invalidate();
        snapshotDirty_.mark();
        publish_snapshot();
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
        const BaseResourceSharingStatus status{
            .enabled = next.enabled,
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
            .runtimeError = runtimeError_,
        };
        next.status = format_status(status);

        const std::lock_guard lock(snapshotMutex_);
        next.configError = snapshot_.configError;
        snapshot_ = std::move(next);
    }

    RuntimeState runtime_;
    RequestGuard requestGuard_;
    BuildUnionWindow buildWindow_;
    std::vector<HookResolution> resolutions_{all_hook_resolutions(false)};
    std::vector<HookBinding> hooks_;
    std::vector<RuntimePatch> patches_;
    std::array<std::string, 3> worldDisabledErrors_;
    UFunction* synchronousOwner_{};
    ResourceOperation synchronousOperation_{ResourceOperation::crafting};
    std::uint32_t synchronousDepth_{};
    bool unionActive_{};
    bool restorationFailed_{};
    std::uint64_t activeGeneration_{};
    std::uint64_t capabilitiesGeneration_{};
    std::size_t baseCount_{};
    std::size_t containerCount_{};
    std::string runtimeError_;
    std::chrono::steady_clock::time_point nextHookAttempt_{};
    std::chrono::steady_clock::time_point unionOpenedAt_{};
    PreviewCacheGate previewCacheGate_;
    PreviewCountSnapshot previewCounts_;
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

auto PalBaseResourceBridge::set_config_error(std::string error) -> void {
    impl_->set_config_error(std::move(error));
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
