#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <base_resource_sharing/current_base_resolution.hpp>
#include <base_resource_sharing/pal_base_resource_runtime.hpp>

namespace base_resource_sharing::detail {
using namespace RC;
using namespace RC::Unreal;

namespace {
class FunctionParams {
public:
    explicit FunctionParams(UFunction* function)
        : function_{function},
          storage_(function == nullptr ? 0U : static_cast<std::size_t>(function->GetParmsSize())) {
        if (function_ != nullptr) {
            function_->InitializeStruct(storage_.data());
        }
    }

    ~FunctionParams() {
        if (function_ != nullptr) {
            function_->DestroyStruct(storage_.data());
        }
    }

    FunctionParams(const FunctionParams&) = delete;
    auto operator=(const FunctionParams&) -> FunctionParams& = delete;

    [[nodiscard]] auto data() noexcept -> void* {
        return storage_.data();
    }

private:
    UFunction* function_{};
    std::vector<std::byte> storage_;
};

[[nodiscard]] auto to_key(const FGuid& guid) -> GuidKey {
    return {{guid.A, guid.B, guid.C, guid.D}};
}

[[nodiscard]] auto to_guid(const GuidKey& key) -> FGuid {
    return FGuid{key.words[0], key.words[1], key.words[2], key.words[3]};
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

[[nodiscard]] auto pal_utility() -> UObject* {
    return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr,
                                                      STR("/Script/Pal.Default__PalUtility"));
}

[[nodiscard]] auto call_utility_bool(UObject* worldContext, const CharType* functionName,
                                     bool& value) -> bool {
    value = false;
    auto* utility = pal_utility();
    auto* function = utility == nullptr ? nullptr : utility->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return false;
    }
    struct Params {
        UObject* WorldContextObject{};
        bool ReturnValue{};
    } params{.WorldContextObject = worldContext};
    utility->ProcessEvent(function, &params);
    value = params.ReturnValue;
    return true;
}

[[nodiscard]] auto call_utility_object(UObject* worldContext, const CharType* functionName,
                                       UObject*& value) -> bool {
    value = nullptr;
    auto* utility = pal_utility();
    auto* function = utility == nullptr ? nullptr : utility->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return false;
    }
    struct Params {
        UObject* WorldContextObject{};
        UObject* ReturnValue{};
    } params{.WorldContextObject = worldContext};
    utility->ProcessEvent(function, &params);
    value = params.ReturnValue;
    return value != nullptr;
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

[[nodiscard]] auto read_object_property(UObject* object, const CharType* propertyName) -> UObject* {
    auto* property =
        object == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(object->GetPropertyByNameInChain(propertyName));
    return property == nullptr
               ? nullptr
               : property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(object));
}

[[nodiscard]] auto try_get_player_guild(UObject* worldContext, const FGuid& playerId,
                                        UObject*& guild) -> bool {
    guild = nullptr;
    auto* utility = pal_utility();
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
    UObject* controller{};
    if (!call_utility_object(worldContext, STR("GetLocalPalPlayerController"), controller)) {
        return false;
    }

    FGuid playerId{};
    UObject* guild{};
    return try_get_guid(controller, STR("GetPlayerUId"), playerId) &&
           try_get_player_guild(controller, playerId, guild) &&
           try_get_guid(guild, STR("GetId"), guildId);
}

[[nodiscard]] auto read_base_ids(UObject* manager, std::vector<FGuid>& output) -> bool {
    output.clear();
    auto* function =
        manager == nullptr ? nullptr : manager->GetFunctionByNameInChain(STR("GetBaseCampIds"));
    auto* arrayProperty =
        function == nullptr
            ? nullptr
            : CastField<FArrayProperty>(function->FindProperty(FName(STR("OutIds"), FNAME_Find)));
    auto* guidProperty =
        arrayProperty == nullptr ? nullptr : CastField<FStructProperty>(arrayProperty->GetInner());
    if (function == nullptr || arrayProperty == nullptr || guidProperty == nullptr) {
        return false;
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

    manager->ProcessEvent(function, params.data());
    FScriptArrayHelper_InContainer values(arrayProperty, params.data());
    output.reserve(static_cast<std::size_t>(std::max(values.Num(), 0)));
    for (int32 index{}; index < values.Num(); ++index) {
        FGuid id{};
        guidProperty->CopyCompleteValue(&id, values.GetRawPtr(index));
        if (to_key(id).valid()) {
            output.push_back(id);
        }
    }
    return true;
}

[[nodiscard]] auto try_get_base_model(UObject* manager, const FGuid& baseId, UObject*& model)
    -> bool {
    model = nullptr;
    auto* function =
        manager == nullptr ? nullptr : manager->GetFunctionByNameInChain(STR("TryGetModel"));
    auto* idProperty = function == nullptr ? nullptr
                                           : CastField<FStructProperty>(function->FindProperty(
                                                 FName(STR("BaseCampId"), FNAME_Find)));
    auto* modelProperty = function == nullptr
                              ? nullptr
                              : CastField<FObjectPropertyBase>(
                                    function->FindProperty(FName(STR("OutModel"), FNAME_Find)));
    auto* returnProperty =
        function == nullptr ? nullptr : CastField<FBoolProperty>(function->GetReturnProperty());
    if (function == nullptr || idProperty == nullptr || modelProperty == nullptr ||
        returnProperty == nullptr) {
        return false;
    }

    std::vector<std::byte> params(function->GetParmsSize());
    idProperty->CopyCompleteValue(idProperty->ContainerPtrToValuePtr<void>(params.data()), &baseId);
    manager->ProcessEvent(function, params.data());
    model = modelProperty->GetObjectPropertyValue(
        modelProperty->ContainerPtrToValuePtr<void>(params.data()));
    return returnProperty->GetPropertyValueInContainer(params.data()) && model != nullptr;
}

[[nodiscard]] auto find_concrete_model(UObject* manager, const GuidKey& ownerMapObjectId)
    -> UObject* {
    if (manager == nullptr || !ownerMapObjectId.valid()) {
        return nullptr;
    }
    auto* function = manager->GetFunctionByNameInChain(STR("FindConcreteModel"));
    if (function == nullptr) {
        return nullptr;
    }
    struct Params {
        FGuid InstanceId{};
        UObject* ReturnValue{};
    } params{.InstanceId = to_guid(ownerMapObjectId)};
    manager->ProcessEvent(function, &params);
    return params.ReturnValue;
}

[[nodiscard]] auto try_get_container_info(FArrayProperty* arrayProperty, void* containerInfo,
                                          FGuid& containerId, FGuid& ownerMapObjectId,
                                          std::uint8_t* containerType = nullptr) -> bool {
    auto* infoProperty =
        arrayProperty == nullptr ? nullptr : CastField<FStructProperty>(arrayProperty->GetInner());
    auto* infoStruct = infoProperty == nullptr ? nullptr : infoProperty->GetStruct().Get();
    auto* ownerProperty = infoStruct == nullptr
                              ? nullptr
                              : CastField<FStructProperty>(infoStruct->GetPropertyByNameInChain(
                                    STR("OwnerMapObjectConcreteModelInstanceId")));
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
    auto* typeProperty =
        infoStruct == nullptr ? nullptr : infoStruct->GetPropertyByNameInChain(STR("Type"));
    if (containerInfo == nullptr || ownerProperty == nullptr || containerIdProperty == nullptr ||
        idProperty == nullptr || (containerType != nullptr && typeProperty == nullptr)) {
        return false;
    }

    ownerProperty->CopyCompleteValue(&ownerMapObjectId,
                                     ownerProperty->ContainerPtrToValuePtr<void>(containerInfo));
    auto* cachedId = containerIdProperty->ContainerPtrToValuePtr<void>(containerInfo);
    idProperty->CopyCompleteValue(&containerId, idProperty->ContainerPtrToValuePtr<void>(cachedId));
    if (containerType != nullptr) {
        typeProperty->CopyCompleteValue(containerType,
                                        typeProperty->ContainerPtrToValuePtr<void>(containerInfo));
    }
    return to_key(containerId).valid() && to_key(ownerMapObjectId).valid();
}

[[nodiscard]] auto read_module_sequence(UObject* module, FArrayProperty* property,
                                        std::vector<GuidKey>& output) -> bool {
    output.clear();
    if (module == nullptr || property == nullptr) {
        return false;
    }
    FScriptArrayHelper_InContainer infos(property, module);
    output.reserve(static_cast<std::size_t>(std::max(infos.Num(), 0)));
    for (int32 index{}; index < infos.Num(); ++index) {
        FGuid containerId{};
        FGuid ownerMapObjectId{};
        if (!try_get_container_info(property, infos.GetRawPtr(index), containerId,
                                    ownerMapObjectId)) {
            return false;
        }
        output.push_back(to_key(containerId));
    }
    return true;
}

[[nodiscard]] auto read_helper_sequence(UObject* helper, FArrayProperty* property,
                                        std::vector<GuidKey>& output) -> bool {
    output.clear();
    auto* objectProperty =
        property == nullptr ? nullptr : CastField<FObjectPropertyBase>(property->GetInner());
    if (helper == nullptr || property == nullptr || objectProperty == nullptr) {
        return false;
    }
    FScriptArrayHelper_InContainer containers(property, helper);
    output.reserve(static_cast<std::size_t>(std::max(containers.Num(), 0)));
    for (int32 index{}; index < containers.Num(); ++index) {
        auto* container = objectProperty->GetObjectPropertyValue(containers.GetRawPtr(index));
        FGuid id{};
        if (!try_get_guid(container, STR("GetId"), id)) {
            return false;
        }
        output.push_back(to_key(id));
    }
    return true;
}

[[nodiscard]] auto append_array_copy(FArrayProperty* property, UObject* target, const void* source)
    -> bool {
    if (property == nullptr || target == nullptr || source == nullptr) {
        return false;
    }
    auto* inner = property->GetInner();
    auto* array = property->ContainerPtrToValuePtr<FScriptArray>(target);
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

[[nodiscard]] auto remove_array_indices(FArrayProperty* property, UObject* target,
                                        std::vector<int32> indices) -> bool {
    if (property == nullptr || target == nullptr) {
        return false;
    }
    auto* inner = property->GetInner();
    auto* array = property->ContainerPtrToValuePtr<FScriptArray>(target);
    if (inner == nullptr || array == nullptr) {
        return false;
    }
    std::ranges::sort(indices, std::greater{});
    for (const auto index : indices) {
        if (index < 0 || index >= array->Num()) {
            return false;
        }
        auto* element = static_cast<std::uint8_t*>(array->GetData()) +
                        static_cast<std::size_t>(index) * inner->GetElementSize();
        inner->DestroyValue(element);
        array->Remove(index, 1, inner->GetElementSize(), inner->GetMinAlignment());
    }
    return true;
}

auto notify_array_changed(UObject* object, const CharType* functionName) -> void {
    if (object == nullptr) {
        return;
    }
    if (auto* function = object->GetFunctionByNameInChain(functionName); function != nullptr) {
        object->ProcessEvent(function, nullptr);
    }
}

[[nodiscard]] auto resolve_live_container(UObject* mapObjectManager, const CatalogContainer& entry)
    -> UObject* {
    auto* concrete = find_concrete_model(mapObjectManager, entry.ownerMapObjectId);
    UObject* itemContainerModule{};
    UObject* container{};
    FGuid resolvedId{};
    if (!try_get_object(concrete, STR("GetItemContainerModule"), itemContainerModule) ||
        !try_get_object(itemContainerModule, STR("GetContainer"), container) ||
        !try_get_guid(container, STR("GetId"), resolvedId) ||
        to_key(resolvedId) != entry.containerId) {
        return nullptr;
    }
    return container;
}

[[nodiscard]] auto locate_catalog_container(const ResourceCatalogSnapshot& catalog,
                                            const GuidKey& containerId)
    -> std::pair<const CatalogModule*, const CatalogContainer*> {
    for (const auto& module : catalog.modules) {
        const auto found =
            std::ranges::find(module.containers, containerId, &CatalogContainer::containerId);
        if (found != module.containers.end()) {
            return {&module, &*found};
        }
    }
    return {};
}

[[nodiscard]] auto append_container_info(UObject* targetModule, FArrayProperty* targetProperty,
                                         const CatalogModule& sourceModule,
                                         const GuidKey& containerId) -> bool {
    auto* sourceObject = find_object_by_full_name(sourceModule.objectFullName);
    auto* sourceProperty = sourceObject == nullptr
                               ? nullptr
                               : CastField<FArrayProperty>(
                                     sourceObject->GetPropertyByNameInChain(STR("ContainerInfos")));
    if (targetModule == nullptr || targetProperty == nullptr || sourceProperty == nullptr) {
        return false;
    }
    FScriptArrayHelper_InContainer infos(sourceProperty, sourceObject);
    for (int32 index{}; index < infos.Num(); ++index) {
        FGuid id{};
        FGuid ownerMapObjectId{};
        if (try_get_container_info(sourceProperty, infos.GetRawPtr(index), id, ownerMapObjectId) &&
            to_key(id) == containerId) {
            return append_array_copy(targetProperty, targetModule, infos.GetRawPtr(index));
        }
    }
    return false;
}

[[nodiscard]] auto indices_removed_from_sequences(const std::span<const GuidKey> current,
                                                  const std::span<const GuidKey> kept,
                                                  std::vector<int32>& indices) -> bool {
    indices.clear();
    std::size_t keptIndex{};
    for (std::size_t index{}; index < current.size(); ++index) {
        if (keptIndex < kept.size() && current[index] == kept[keptIndex]) {
            ++keptIndex;
        } else {
            indices.push_back(static_cast<int32>(index));
        }
    }
    return keptIndex == kept.size();
}

[[nodiscard]] auto restore_entry(const UnionLedgerEntry& entry) -> bool {
    auto* object = find_object_by_full_name(entry.objectFullName);
    if (object == nullptr) {
        return true;
    }
    const auto* propertyName = entry.helperArray ? STR("Containers") : STR("ContainerInfos");
    auto* property = CastField<FArrayProperty>(object->GetPropertyByNameInChain(propertyName));
    std::vector<GuidKey> current;
    const bool read = entry.helperArray ? read_helper_sequence(object, property, current)
                                        : read_module_sequence(object, property, current);
    if (!read) {
        return false;
    }
    const auto removal = remove_recorded_injections(current, entry.original, entry.injected);
    std::vector<int32> indices;
    if (!removal.complete || !indices_removed_from_sequences(current, removal.kept, indices) ||
        !remove_array_indices(property, object, std::move(indices))) {
        return false;
    }
    notify_array_changed(object,
                         entry.helperArray ? STR("OnRep_Containers") : STR("OnRep_ContainerInfos"));
    std::vector<GuidKey> restored;
    const bool finalRead = entry.helperArray ? read_helper_sequence(object, property, restored)
                                             : read_module_sequence(object, property, restored);
    return finalRead && restored == removal.kept;
}

[[nodiscard]] auto sequence_status_text(const SequenceValidationStatus status) -> std::string_view {
    switch (status) {
        case SequenceValidationStatus::valid:
            return "valid";
        case SequenceValidationStatus::originalPrefixChanged:
            return "originalPrefixChanged";
        case SequenceValidationStatus::injectedCountMismatch:
            return "injectedCountMismatch";
        case SequenceValidationStatus::duplicateInjectedId:
            return "duplicateInjectedId";
        case SequenceValidationStatus::unexpectedTail:
            return "unexpectedTail";
    }
    return "unknown";
}
}  // namespace

auto local_authority_ready(UObject* worldContext, std::string& error) -> bool {
    bool isServer{};
    bool isDedicated{};
    if (worldContext == nullptr || !call_utility_bool(worldContext, STR("IsServer"), isServer) ||
        !call_utility_bool(worldContext, STR("IsDedicatedServer"), isDedicated)) {
        error = "无法确认当前世界的本地房主身份。";
        return false;
    }
    if (!isServer || isDedicated) {
        error = "据点资源共享仅支持单人世界或本地房主。";
        return false;
    }
    error.clear();
    return true;
}

auto read_base_id(UObject* baseModel) -> std::optional<GuidKey> {
    using Names = CurrentBaseReflectionNames<CharType>;
    FGuid value{};
    if (!try_get_guid(baseModel, Names::baseIdFunction.data(), value)) {
        return std::nullopt;
    }
    const auto key = to_key(value);
    return key.valid() ? std::optional{key} : std::nullopt;
}

auto resolve_inside_base_id(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                            std::string& error) -> std::optional<GuidKey> {
    using Names = CurrentBaseReflectionNames<CharType>;
    error.clear();

    UObject* controller{};
    if (!call_utility_object(worldContext, STR("GetLocalPalPlayerController"), controller)) {
        error = "无法解析本地玩家控制器。";
        return std::nullopt;
    }

    UObject* pawn{};
    if (!try_get_object(controller, Names::controllerPawnFunction.data(), pawn)) {
        error = "本地玩家控制器无法通过 K2_GetPawn 返回 Pawn。";
        return std::nullopt;
    }

    auto* insideComponent = read_object_property(pawn, Names::insideComponentProperty.data());
    if (insideComponent == nullptr) {
        error = "本地 Pawn 缺少 InsideBaseCampCheckComponent。";
        return std::nullopt;
    }

    UObject* baseModel{};
    if (!try_get_object(insideComponent, Names::insideBaseModelFunction.data(), baseModel)) {
        error = "当前不在游戏已确认的据点内。";
        return std::nullopt;
    }

    const auto candidate = read_base_id(baseModel);
    if (!candidate.has_value()) {
        error = "当前据点模型的 GetId 未返回有效 GUID。";
        return std::nullopt;
    }

    const bool hasStorageModule = std::ranges::any_of(
        catalog.modules, [&](const auto& module) { return module.baseId == *candidate; });
    const auto accepted = accept_current_base(*candidate, hasStorageModule);
    if (!accepted.has_value()) {
        error = "当前据点不在同公会普通仓储目录中。";
    }
    return accepted;
}

auto discover_catalog(UObject* worldContext, const std::uint64_t generation,
                      const std::span<const PersistentUnionEdge> appliedEdges)
    -> ResourceCatalogSnapshot {
    ResourceCatalogSnapshot result{.generation = generation};
    std::string authorityError;
    if (!local_authority_ready(worldContext, authorityError)) {
        result.error = std::move(authorityError);
        return result;
    }

    FGuid guildId{};
    UObject* baseCampManager{};
    UObject* mapObjectManager{};
    if (!try_resolve_local_guild(worldContext, guildId) ||
        !call_utility_object(worldContext, STR("GetBaseCampManager"), baseCampManager) ||
        !call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager)) {
        result.error = "无法解析本地公会、据点管理器或地图物体管理器。";
        return result;
    }
    result.guildId = to_key(guildId);

    auto* storageClass = UObjectGlobals::StaticFindObject<UClass*>(
        nullptr, nullptr, STR("/Script/Pal.PalBaseCampModuleItemStorage"));
    std::vector<FGuid> baseIds;
    if (storageClass == nullptr || !read_base_ids(baseCampManager, baseIds)) {
        result.error = "无法读取据点列表或据点仓储模块类型。";
        return result;
    }

    std::vector<ContainerDescriptor> descriptors;
    std::set<GuidKey> catalogIds;
    for (const auto& baseId : baseIds) {
        UObject* baseModel{};
        FGuid ownerGuild{};
        if (!try_get_base_model(baseCampManager, baseId, baseModel) ||
            !try_get_guid(baseModel, STR("GetGroupIdBelongTo"), ownerGuild)) {
            continue;
        }
        const bool sameGuild = to_key(ownerGuild) == result.guildId;
        if (sameGuild) {
            ++result.sameGuildBaseCount;
        }

        auto* moduleArray =
            CastField<FArrayProperty>(baseModel->GetPropertyByNameInChain(STR("ModuleArray")));
        auto* moduleProperty = moduleArray == nullptr
                                   ? nullptr
                                   : CastField<FObjectPropertyBase>(moduleArray->GetInner());
        if (moduleArray == nullptr || moduleProperty == nullptr) {
            if (sameGuild) {
                result.error = "据点模型缺少 ModuleArray 对象数组。";
                return result;
            }
            continue;
        }

        UObject* storageModule{};
        FScriptArrayHelper_InContainer modules(moduleArray, baseModel);
        for (int32 index{}; index < modules.Num(); ++index) {
            auto* candidate = moduleProperty->GetObjectPropertyValue(modules.GetRawPtr(index));
            if (candidate != nullptr && candidate->IsA(storageClass)) {
                storageModule = candidate;
                break;
            }
        }
        if (storageModule == nullptr) {
            continue;
        }

        const auto storageModuleName = object_name(storageModule);
        if (!sameGuild) {
            result.ignoredModuleNames.push_back(storageModuleName);
            continue;
        }

        auto* containerInfos = CastField<FArrayProperty>(
            storageModule->GetPropertyByNameInChain(STR("ContainerInfos")));
        if (containerInfos == nullptr) {
            result.error = "据点仓储模块缺少 ContainerInfos。";
            return result;
        }

        CatalogModule catalogModule{.baseId = to_key(baseId),
                                    .objectFullName = std::move(storageModuleName)};
        std::set<GuidKey> moduleIds;
        FScriptArrayHelper_InContainer infos(containerInfos, storageModule);
        for (int32 index{}; index < infos.Num(); ++index) {
            FGuid containerId{};
            FGuid ownerMapObjectId{};
            std::uint8_t containerType{};
            if (!try_get_container_info(containerInfos, infos.GetRawPtr(index), containerId,
                                        ownerMapObjectId, &containerType)) {
                result.error = "普通箱子登记项缺少有效容器或地图物体标识。";
                return result;
            }
            const CatalogContainer entry{.containerId = to_key(containerId),
                                         .ownerMapObjectId = to_key(ownerMapObjectId)};
            const ConcreteModelRegistrationKey registration{
                .moduleFullName = catalogModule.objectFullName,
                .ownerMapObjectId = entry.ownerMapObjectId,
            };
            if (containerType != 0) {
                result.ignoredRegistrations.push_back(registration);
                continue;
            }
            result.registrations.push_back(registration);
            const auto appliedEdge = std::ranges::find_if(appliedEdges, [&](const auto& edge) {
                return edge.targetBaseId == catalogModule.baseId &&
                       edge.targetModuleFullName == catalogModule.objectFullName &&
                       edge.containerId == entry.containerId &&
                       edge.ownerMapObjectId == entry.ownerMapObjectId;
            });
            if (appliedEdge != appliedEdges.end()) {
                result.observedAppliedEdges.push_back(*appliedEdge);
                continue;
            }
            if (moduleIds.insert(entry.containerId).second) {
                const bool firstGlobalOccurrence = catalogIds.insert(entry.containerId).second;
                if (firstGlobalOccurrence) {
                    ++result.registeredContainerCount;
                }
                if (resolve_live_container(mapObjectManager, entry) == nullptr) {
                    if (firstGlobalOccurrence) {
                        ++result.pendingContainerCount;
                    }
                    continue;
                }
                catalogModule.containers.push_back(entry);
                if (firstGlobalOccurrence) {
                    descriptors.push_back({.baseId = catalogModule.baseId,
                                           .groupId = result.guildId,
                                           .containerId = entry.containerId,
                                           .kind = ContainerKind::normal});
                }
            }
        }
        result.modules.push_back(std::move(catalogModule));
    }

    result.observedAppliedEdges = normalized_edges(result.observedAppliedEdges);

    if (!descriptors.empty()) {
        result.plan = make_resource_union_plan(descriptors, result.guildId);
        if (!result.plan.error.empty()) {
            result.error = result.plan.error;
            return result;
        }
    }
    result.initialized = true;
    return result;
}

auto apply_union(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                 const ResourceExposurePlan& exposure, LiveUnion& liveUnion, std::string& error)
    -> bool {
    error.clear();
    if (liveUnion.active) {
        error = "已有据点资源联合处于活动状态。";
        return false;
    }
    if (exposure.surface == ResourceConsumerSurface::none ||
        exposure.operation == ResourceOperation::repair) {
        error = "当前材料操作没有可用的单一消费入口。";
        return false;
    }
    if (worldContext == nullptr || !catalog.initialized || catalog.generation == 0 ||
        !catalog.guildId.valid() || !catalog.error.empty() || catalog.plan.ordered.empty()) {
        error = "资源目录尚未完成安全校准。";
        return false;
    }

    liveUnion = {
        .generation = catalog.generation,
        .guildId = catalog.guildId,
        .exposure = exposure,
        .active = true,
    };
    const auto rollback = [&](std::string primaryError) {
        std::string restoreError;
        if (!restore_union(liveUnion, restoreError)) {
            primaryError += "；" + restoreError;
        }
        error = std::move(primaryError);
        return false;
    };

    if (!exposure.targetBaseId.has_value()) {
        return rollback("材料联合缺少当前据点标识。");
    }
    const auto globalIds =
        select_shared_container_ids(catalog.plan.ordered, *exposure.targetBaseId);

    if (exposure.surface == ResourceConsumerSurface::currentBaseModule) {
        const auto module =
            std::ranges::find(catalog.modules, *exposure.targetBaseId, &CatalogModule::baseId);
        if (module == catalog.modules.end()) {
            return rollback("当前据点没有可用的普通仓储模块。");
        }
        auto* object = find_object_by_full_name(module->objectFullName);
        auto* property = object == nullptr
                             ? nullptr
                             : CastField<FArrayProperty>(
                                   object->GetPropertyByNameInChain(STR("ContainerInfos")));
        UnionLedgerEntry ledger{.objectFullName = module->objectFullName};
        if (!read_module_sequence(object, property, ledger.original)) {
            return rollback("建立建造联合时无法读取当前据点仓储序列。");
        }
        liveUnion.entry = std::move(ledger);
        const auto missing = missing_union_tail(liveUnion.entry->original, globalIds);
        for (const auto& id : missing) {
            const auto [sourceModule, sourceContainer] = locate_catalog_container(catalog, id);
            static_cast<void>(sourceContainer);
            if (sourceModule == nullptr ||
                !append_container_info(object, property, *sourceModule, id)) {
                return rollback("向当前据点仓储追加共享箱子登记失败。");
            }
            liveUnion.entry->injected.push_back(id);
        }
        if (!liveUnion.entry->injected.empty()) {
            notify_array_changed(object, STR("OnRep_ContainerInfos"));
        }
        std::vector<GuidKey> current;
        if (!read_module_sequence(object, property, current)) {
            return rollback("无法重读建造联合后的当前据点仓储序列。");
        }
        const auto validation = validate_applied_sequence(liveUnion.entry->original,
                                                          liveUnion.entry->injected, current);
        if (!validation) {
            return rollback("建造联合序列验证失败：" +
                            std::string{sequence_status_text(validation.status)});
        }
        return true;
    }

    UObject* mapObjectManager{};
    UObject* inventoryData{};
    if (!call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager) ||
        !call_utility_object(worldContext, STR("GetLocalInventoryData"), inventoryData)) {
        return rollback("无法解析地图物体管理器或本地主背包。");
    }
    auto* helperProperty = CastField<FObjectPropertyBase>(
        inventoryData->GetPropertyByNameInChain(STR("InventoryMultiHelper")));
    auto* helper = helperProperty == nullptr
                       ? nullptr
                       : helperProperty->GetObjectPropertyValue(
                             helperProperty->ContainerPtrToValuePtr<void>(inventoryData));
    auto* containersProperty =
        helper == nullptr
            ? nullptr
            : CastField<FArrayProperty>(helper->GetPropertyByNameInChain(STR("Containers")));
    auto* containerProperty = containersProperty == nullptr
                                  ? nullptr
                                  : CastField<FObjectPropertyBase>(containersProperty->GetInner());
    UnionLedgerEntry helperLedger{
        .objectFullName = object_name(helper),
        .helperArray = true,
    };
    if (containerProperty == nullptr ||
        !read_helper_sequence(helper, containersProperty, helperLedger.original)) {
        return rollback("本地主背包资源助手不可用。");
    }
    liveUnion.entry = std::move(helperLedger);

    const auto missingHelperIds = missing_union_tail(liveUnion.entry->original, globalIds);
    for (const auto& id : missingHelperIds) {
        const auto [sourceModule, sourceContainer] = locate_catalog_container(catalog, id);
        static_cast<void>(sourceModule);
        auto* container = sourceContainer == nullptr
                              ? nullptr
                              : resolve_live_container(mapObjectManager, *sourceContainer);
        if (container == nullptr || !append_array_copy(containersProperty, helper, &container)) {
            return rollback("向本地主背包资源助手追加共享箱子失败。");
        }
        liveUnion.entry->injected.push_back(id);
    }
    if (!liveUnion.entry->injected.empty()) {
        notify_array_changed(helper, STR("OnRep_Containers"));
    }
    std::vector<GuidKey> current;
    if (!read_helper_sequence(helper, containersProperty, current)) {
        return rollback("无法重读制作联合后的本地主背包资源助手序列。");
    }
    const auto validation =
        validate_applied_sequence(liveUnion.entry->original, liveUnion.entry->injected, current);
    if (!validation) {
        return rollback("制作联合序列验证失败：" +
                        std::string{sequence_status_text(validation.status)});
    }
    return true;
}

auto persistent_modules(const ResourceCatalogSnapshot& catalog)
    -> std::vector<PersistentStorageModule> {
    std::vector<PersistentStorageModule> result;
    result.reserve(catalog.modules.size());
    for (const auto& module : catalog.modules) {
        PersistentStorageModule persistent{
            .baseId = module.baseId,
            .objectFullName = module.objectFullName,
        };
        persistent.containers.reserve(module.containers.size());
        for (const auto& container : module.containers) {
            persistent.containers.push_back({
                .containerId = container.containerId,
                .ownerMapObjectId = container.ownerMapObjectId,
            });
        }
        result.push_back(std::move(persistent));
    }
    return result;
}

namespace {
[[nodiscard]] auto persistent_edge_target(const ResourceCatalogSnapshot& catalog,
                                          const PersistentUnionEdge& edge) -> const CatalogModule* {
    const auto target = std::ranges::find_if(catalog.modules, [&](const auto& module) {
        return module.baseId == edge.targetBaseId &&
               module.objectFullName == edge.targetModuleFullName;
    });
    return target == catalog.modules.end() ? nullptr : &*target;
}

[[nodiscard]] auto persistent_edge_source(const ResourceCatalogSnapshot& catalog,
                                          const PersistentUnionEdge& edge)
    -> const CatalogContainer* {
    const auto module =
        std::ranges::find(catalog.modules, edge.sourceBaseId, &CatalogModule::baseId);
    if (module == catalog.modules.end()) {
        return nullptr;
    }
    const auto container = std::ranges::find_if(module->containers, [&](const auto& candidate) {
        return candidate.containerId == edge.containerId &&
               candidate.ownerMapObjectId == edge.ownerMapObjectId;
    });
    return container == module->containers.end() ? nullptr : &*container;
}

[[nodiscard]] auto module_property(UObject* module) -> FArrayProperty* {
    return module == nullptr
               ? nullptr
               : CastField<FArrayProperty>(module->GetPropertyByNameInChain(STR("ContainerInfos")));
}

[[nodiscard]] auto count_container(const std::span<const GuidKey> sequence,
                                   const GuidKey& containerId) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count(sequence, containerId));
}

[[nodiscard]] auto call_concrete_model_event(UObject* module, const CharType* functionName,
                                             UObject* concreteModel) -> bool {
    auto* function = module == nullptr ? nullptr : module->GetFunctionByNameInChain(functionName);
    auto* concreteProperty =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(
                                  function->FindProperty(FName(STR("ConcreteModel"), FNAME_Find)));
    if (function == nullptr || concreteProperty == nullptr || concreteModel == nullptr ||
        !concreteProperty->HasAnyPropertyFlags(CPF_Parm)) {
        return false;
    }
    FunctionParams params{function};
    concreteProperty->SetObjectPropertyValue(
        concreteProperty->ContainerPtrToValuePtr<void>(params.data()), concreteModel);
    module->ProcessEvent(function, params.data());
    return true;
}

[[nodiscard]] auto remove_container_id_fallback(UObject* module, FArrayProperty* property,
                                                const GuidKey& containerId,
                                                const std::span<const GuidKey> before) -> bool {
    const auto position = std::ranges::find(before, containerId);
    if (position == before.end()) {
        return true;
    }
    const auto index = static_cast<int32>(std::distance(before.begin(), position));
    if (!remove_array_indices(property, module, {index})) {
        return false;
    }
    notify_array_changed(module, STR("OnRep_ContainerInfos"));
    std::vector<GuidKey> after;
    if (!read_module_sequence(module, property, after)) {
        return false;
    }
    std::vector<GuidKey> expected{before.begin(), before.end()};
    expected.erase(expected.begin() + index);
    return after == expected;
}
}  // namespace

auto apply_persistent_edge(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                           const PersistentUnionEdge& edge) -> PersistentEdgeMutationResult {
    if (worldContext == nullptr || catalog.generation == 0 ||
        persistent_edge_target(catalog, edge) == nullptr ||
        persistent_edge_source(catalog, edge) == nullptr) {
        return {.error = "持久联合登记边不属于当前安全目录。"};
    }
    auto* module = find_object_by_full_name(edge.targetModuleFullName);
    auto* property = module_property(module);
    std::vector<GuidKey> before;
    if (!read_module_sequence(module, property, before)) {
        return {.error = "持久联合登记前无法读取目标据点仓储序列。"};
    }
    const auto beforeCount = count_container(before, edge.containerId);
    if (beforeCount == 1) {
        return {.mutation = PersistentEdgeMutation::unchanged};
    }
    if (beforeCount != 0) {
        return {.error = "持久联合登记前目标容器已重复。"};
    }

    UObject* mapObjectManager{};
    if (!call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager)) {
        return {.error = "持久联合登记时无法解析地图物体管理器。"};
    }
    auto* concreteModel = find_concrete_model(mapObjectManager, edge.ownerMapObjectId);
    if (!call_concrete_model_event(module, STR("OnAvailableConcreteModel_ServerInternal"),
                                   concreteModel)) {
        return {.error = "无法调用原生仓储 ConcreteModel 登记接口。"};
    }

    std::vector<GuidKey> after;
    if (!read_module_sequence(module, property, after)) {
        static_cast<void>(call_concrete_model_event(
            module, STR("OnNotAvailableConcreteModel_ServerInternal"), concreteModel));
        return {.mutation = PersistentEdgeMutation::added,
                .error = "持久联合登记后无法重读目标据点仓储序列，已请求原生回滚但无法验证。"};
    }
    const auto mutation =
        classify_persistent_edge_add(beforeCount, count_container(after, edge.containerId));
    auto expected = before;
    expected.push_back(edge.containerId);
    if (mutation != PersistentEdgeMutation::added || after != expected) {
        static_cast<void>(call_concrete_model_event(
            module, STR("OnNotAvailableConcreteModel_ServerInternal"), concreteModel));
        std::vector<GuidKey> restored;
        if (!read_module_sequence(module, property, restored) || restored != before) {
            return {.mutation = PersistentEdgeMutation::added,
                    .error = "原生仓储登记序列异常，且回滚验证失败。"};
        }
        return {.error = "原生仓储登记未产生精确的追加变化，已完整回滚。"};
    }
    return {.mutation = mutation};
}

auto remove_persistent_edge(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                            const PersistentUnionEdge& edge) -> PersistentEdgeMutationResult {
    static_cast<void>(catalog);
    if (edge.targetModuleFullName.empty() || !edge.containerId.valid()) {
        return {.error = "持久联合注销边无效。"};
    }
    auto* module = find_object_by_full_name(edge.targetModuleFullName);
    if (module == nullptr) {
        return {.mutation = PersistentEdgeMutation::removed};
    }
    auto* property = module_property(module);
    std::vector<GuidKey> before;
    if (!read_module_sequence(module, property, before)) {
        return {.error = "持久联合注销前无法读取目标据点仓储序列。"};
    }
    const auto beforeCount = count_container(before, edge.containerId);
    if (beforeCount == 0) {
        return {.mutation = PersistentEdgeMutation::unchanged};
    }
    if (beforeCount != 1) {
        return {.error = "持久联合注销前目标容器出现次数异常。"};
    }

    UObject* mapObjectManager{};
    UObject* concreteModel{};
    if (worldContext != nullptr &&
        call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager)) {
        concreteModel = find_concrete_model(mapObjectManager, edge.ownerMapObjectId);
    }
    if (concreteModel == nullptr) {
        if (!remove_container_id_fallback(module, property, edge.containerId, before)) {
            return {.error = "ConcreteModel 已卸载，精确数组恢复也失败。"};
        }
        return {.mutation = PersistentEdgeMutation::removed};
    }
    if (!call_concrete_model_event(module, STR("OnNotAvailableConcreteModel_ServerInternal"),
                                   concreteModel)) {
        return {.error = "无法调用原生仓储 ConcreteModel 注销接口。"};
    }

    std::vector<GuidKey> after;
    if (!read_module_sequence(module, property, after)) {
        return {.error = "持久联合注销后无法重读目标据点仓储序列。"};
    }
    const auto mutation =
        classify_persistent_edge_remove(beforeCount, count_container(after, edge.containerId));
    if (mutation != PersistentEdgeMutation::removed) {
        return {.error = "原生仓储注销未产生精确的 1→0 容器变化。"};
    }
    std::vector<GuidKey> expected = before;
    std::erase(expected, edge.containerId);
    if (after != expected) {
        return {.mutation = PersistentEdgeMutation::removed,
                .error = "原生仓储注销改变了非注入容器序列。"};
    }
    return {.mutation = mutation};
}

auto validate_union(const LiveUnion& liveUnion, std::string& error) -> bool {
    error.clear();
    if (!liveUnion.active || !liveUnion.entry.has_value()) {
        error = "材料提交前缺少活动联合账本。";
        return false;
    }

    auto* object = find_object_by_full_name(liveUnion.entry->objectFullName);
    const auto* propertyName =
        liveUnion.entry->helperArray ? STR("Containers") : STR("ContainerInfos");
    auto* property =
        object == nullptr
            ? nullptr
            : CastField<FArrayProperty>(object->GetPropertyByNameInChain(propertyName));
    std::vector<GuidKey> current;
    const bool read = liveUnion.entry->helperArray
                          ? read_helper_sequence(object, property, current)
                          : read_module_sequence(object, property, current);
    if (!read) {
        error = "材料提交前无法重读活动联合序列。";
        return false;
    }
    const auto validation =
        validate_applied_sequence(liveUnion.entry->original, liveUnion.entry->injected, current);
    if (!validation) {
        error =
            "材料提交前联合序列验证失败：" + std::string{sequence_status_text(validation.status)};
        return false;
    }
    return true;
}

auto notify_building_inventory_changed(UObject* buildModel, UObject* worldContext,
                                       const ResourceCatalogSnapshot& catalog,
                                       const LiveUnion& liveUnion, std::string& error) -> bool {
    error.clear();
    if (buildModel == nullptr || worldContext == nullptr ||
        select_building_inventory_refresh_target(liveUnion.active, liveUnion.exposure) !=
            BuildingInventoryRefreshTarget::buildModel ||
        !liveUnion.entry.has_value() || liveUnion.entry->helperArray) {
        error = "建造库存更新通知缺少活动的当前据点资源联合。";
        return false;
    }
    if (liveUnion.entry->injected.empty()) {
        return true;
    }

    auto* function = buildModel->GetFunctionByNameInChain(STR("OnUpdateInventory"));
    auto* containerProperty =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(
                                  function->FindProperty(FName(STR("Container"), FNAME_Find)));
    if (function == nullptr || containerProperty == nullptr ||
        !containerProperty->HasAnyPropertyFlags(CPF_Parm)) {
        error = "建造模型缺少 OnUpdateInventory(Container) 接口。";
        return false;
    }

    UObject* mapObjectManager{};
    if (!call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager)) {
        error = "建造库存更新时无法解析地图物体管理器。";
        return false;
    }

    UObject* changedContainer{};
    for (const auto& id : liveUnion.entry->injected) {
        const auto [module, container] = locate_catalog_container(catalog, id);
        static_cast<void>(module);
        if (container != nullptr) {
            changedContainer = resolve_live_container(mapObjectManager, *container);
        }
        if (changedContainer != nullptr) {
            break;
        }
    }
    if (changedContainer == nullptr) {
        error = "建造库存更新时无法重新解析任一已注入容器。";
        return false;
    }

    FunctionParams params{function};
    containerProperty->SetObjectPropertyValue(
        containerProperty->ContainerPtrToValuePtr<void>(params.data()), changedContainer);
    buildModel->ProcessEvent(function, params.data());
    return true;
}

auto restore_union(LiveUnion& liveUnion, std::string& error) -> bool {
    if (!liveUnion.active) {
        error.clear();
        return true;
    }

    const bool restored = !liveUnion.entry.has_value() || restore_entry(*liveUnion.entry);
    liveUnion = {};
    if (restored) {
        error.clear();
    } else {
        error = "未能验证据点资源联合已完整恢复。";
    }
    return restored;
}
}  // namespace base_resource_sharing::detail
