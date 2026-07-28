#include "pal_base_resource_runtime.hpp"

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
    auto* property =
        baseModel == nullptr
            ? nullptr
            : CastField<FStructProperty>(baseModel->GetPropertyByNameInChain(STR("BaseCampId")));
    if (property == nullptr || property->GetSize() != static_cast<int32>(sizeof(FGuid))) {
        return std::nullopt;
    }

    FGuid value{};
    property->CopyCompleteValue(&value, property->ContainerPtrToValuePtr<void>(baseModel));
    const auto key = to_key(value);
    return key.valid() ? std::optional{key} : std::nullopt;
}

auto resolve_nearest_base_id(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                             std::string& error) -> std::optional<GuidKey> {
    error.clear();
    UObject* controller{};
    UObject* pawn{};
    if (!call_utility_object(worldContext, STR("GetLocalPalPlayerController"), controller) ||
        !try_get_object(controller, STR("GetPawn"), pawn)) {
        error = "无法解析本地玩家控制器或 Pawn。";
        return std::nullopt;
    }

    auto* locationFunction = pawn->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
    auto* locationReturn = locationFunction == nullptr
                               ? nullptr
                               : CastField<FStructProperty>(locationFunction->GetReturnProperty());
    if (locationFunction == nullptr || locationReturn == nullptr ||
        locationReturn->GetSize() != FVector::StaticSize()) {
        error = "K2_GetActorLocation 缺少兼容的 FVector 返回值。";
        return std::nullopt;
    }

    FVector location{};
    {
        FunctionParams params{locationFunction};
        pawn->ProcessEvent(locationFunction, params.data());
        locationReturn->CopyCompleteValue(
            &location, locationReturn->ContainerPtrToValuePtr<void>(params.data()));
    }

    UObject* manager{};
    if (!call_utility_object(worldContext, STR("GetBaseCampManager"), manager)) {
        error = "无法解析据点管理器。";
        return std::nullopt;
    }
    auto* nearestFunction = manager->GetFunctionByNameInChain(STR("GetNearestBaseCamp"));
    FStructProperty* locationInput{};
    FObjectPropertyBase* baseReturn{};
    if (nearestFunction != nullptr) {
        for (auto* property :
             TFieldRange<FProperty>(nearestFunction, EFieldIterationFlags::IncludeDeprecated)) {
            if (!property->HasAnyPropertyFlags(CPF_Parm)) {
                continue;
            }
            if (property->HasAnyPropertyFlags(CPF_ReturnParm)) {
                baseReturn = CastField<FObjectPropertyBase>(property);
                continue;
            }
            auto* candidate = CastField<FStructProperty>(property);
            if (candidate != nullptr && candidate->GetSize() == FVector::StaticSize()) {
                if (locationInput != nullptr) {
                    locationInput = nullptr;
                    break;
                }
                locationInput = candidate;
            }
        }
    }
    if (nearestFunction == nullptr || locationInput == nullptr || baseReturn == nullptr) {
        error = "GetNearestBaseCamp 缺少唯一 FVector 参数或对象返回值。";
        return std::nullopt;
    }

    UObject* baseModel{};
    {
        FunctionParams params{nearestFunction};
        locationInput->CopyCompleteValue(locationInput->ContainerPtrToValuePtr<void>(params.data()),
                                         &location);
        manager->ProcessEvent(nearestFunction, params.data());
        baseModel = baseReturn->GetObjectPropertyValue(
            baseReturn->ContainerPtrToValuePtr<void>(params.data()));
    }
    const auto baseId = read_base_id(baseModel);
    if (!baseId.has_value()) {
        error = "最近据点模型缺少有效 BaseCampId。";
        return std::nullopt;
    }
    const auto belongsToCatalog = std::ranges::any_of(
        catalog.modules, [&](const auto& module) { return module.baseId == *baseId; });
    if (!belongsToCatalog) {
        error = "最近据点不在当前同公会普通仓储目录中。";
        return std::nullopt;
    }
    return baseId;
}

auto discover_catalog(UObject* worldContext, const std::uint64_t generation)
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
            !try_get_guid(baseModel, STR("GetGroupIdBelongTo"), ownerGuild) ||
            to_key(ownerGuild) != result.guildId) {
            continue;
        }
        ++result.sameGuildBaseCount;

        auto* moduleArray =
            CastField<FArrayProperty>(baseModel->GetPropertyByNameInChain(STR("ModuleArray")));
        auto* moduleProperty = moduleArray == nullptr
                                   ? nullptr
                                   : CastField<FObjectPropertyBase>(moduleArray->GetInner());
        if (moduleArray == nullptr || moduleProperty == nullptr) {
            result.error = "据点模型缺少 ModuleArray 对象数组。";
            return result;
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

        auto* containerInfos = CastField<FArrayProperty>(
            storageModule->GetPropertyByNameInChain(STR("ContainerInfos")));
        if (containerInfos == nullptr) {
            result.error = "据点仓储模块缺少 ContainerInfos。";
            return result;
        }

        CatalogModule catalogModule{.baseId = to_key(baseId),
                                    .objectFullName = object_name(storageModule)};
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
            if (containerType != 0) {
                continue;
            }
            const CatalogContainer entry{.containerId = to_key(containerId),
                                         .ownerMapObjectId = to_key(ownerMapObjectId)};
            if (catalogIds.insert(entry.containerId).second) {
                if (resolve_live_container(mapObjectManager, entry) == nullptr) {
                    result.error = "至少一个已登记普通箱子尚未解析为有效容器。";
                    return result;
                }
                catalogModule.containers.push_back(entry);
                descriptors.push_back({.baseId = catalogModule.baseId,
                                       .groupId = result.guildId,
                                       .containerId = entry.containerId,
                                       .kind = ContainerKind::normal});
            }
        }
        result.modules.push_back(std::move(catalogModule));
    }

    result.plan = make_resource_union_plan(descriptors, result.guildId);
    if (!result.plan.error.empty()) {
        result.error = result.plan.error;
    }
    return result;
}

auto apply_union(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                 const UnionTargets targets, LiveUnion& liveUnion, std::string& error) -> bool {
    error.clear();
    if (liveUnion.active) {
        error = "已有据点资源联合处于活动状态。";
        return false;
    }
    if (worldContext == nullptr || catalog.generation == 0 || !catalog.guildId.valid() ||
        !catalog.error.empty() || catalog.plan.ordered.empty()) {
        error = "资源目录尚未完成安全校准。";
        return false;
    }

    UObject* mapObjectManager{};
    UObject* inventoryData{};
    if (!call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager) ||
        !call_utility_object(worldContext, STR("GetLocalInventoryData"), inventoryData)) {
        error = "无法解析地图物体管理器或本地主背包。";
        return false;
    }

    liveUnion = {
        .generation = catalog.generation,
        .guildId = catalog.guildId,
        .targets = targets,
        .active = true,
    };
    std::vector<GuidKey> globalIds;
    globalIds.reserve(catalog.plan.ordered.size());
    for (const auto& descriptor : catalog.plan.ordered) {
        globalIds.push_back(descriptor.containerId);
    }

    if (targets.baseModules) {
        for (const auto& module : catalog.modules) {
            auto* object = find_object_by_full_name(module.objectFullName);
            auto* property = object == nullptr
                                 ? nullptr
                                 : CastField<FArrayProperty>(
                                       object->GetPropertyByNameInChain(STR("ContainerInfos")));
            UnionLedgerEntry ledger{.objectFullName = module.objectFullName};
            if (!read_module_sequence(object, property, ledger.original)) {
                error = "建立联合时无法重新读取据点仓储序列。";
                std::string restoreError;
                static_cast<void>(restore_union(liveUnion, restoreError));
                return false;
            }
            liveUnion.entries.push_back(ledger);

            const auto missing = missing_union_tail(ledger.original, globalIds);
            for (const auto& id : missing) {
                const auto [sourceModule, sourceContainer] = locate_catalog_container(catalog, id);
                static_cast<void>(sourceContainer);
                if (sourceModule == nullptr ||
                    !append_container_info(object, property, *sourceModule, id)) {
                    error = "向据点仓储追加共享箱子登记失败。";
                    std::string restoreError;
                    static_cast<void>(restore_union(liveUnion, restoreError));
                    return false;
                }
                liveUnion.entries.back().injected.push_back(id);
            }
            if (!liveUnion.entries.back().injected.empty()) {
                notify_array_changed(object, STR("OnRep_ContainerInfos"));
            }
        }
    }

    if (!targets.playerHelper) {
        return true;
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
        error = "本地主背包资源助手不可用。";
        std::string restoreError;
        static_cast<void>(restore_union(liveUnion, restoreError));
        return false;
    }
    liveUnion.entries.push_back(helperLedger);

    const auto missingHelperIds = missing_union_tail(helperLedger.original, globalIds);
    for (const auto& id : missingHelperIds) {
        const auto [sourceModule, sourceContainer] = locate_catalog_container(catalog, id);
        static_cast<void>(sourceModule);
        auto* container = sourceContainer == nullptr
                              ? nullptr
                              : resolve_live_container(mapObjectManager, *sourceContainer);
        if (container == nullptr || !append_array_copy(containersProperty, helper, &container)) {
            error = "向本地主背包资源助手追加共享箱子失败。";
            std::string restoreError;
            static_cast<void>(restore_union(liveUnion, restoreError));
            return false;
        }
        liveUnion.entries.back().injected.push_back(id);
    }
    if (!liveUnion.entries.back().injected.empty()) {
        notify_array_changed(helper, STR("OnRep_Containers"));
    }
    return true;
}

auto restore_union(LiveUnion& liveUnion, std::string& error) -> bool {
    if (!liveUnion.active) {
        error.clear();
        return true;
    }

    bool restored = true;
    for (auto entry = liveUnion.entries.rbegin(); entry != liveUnion.entries.rend(); ++entry) {
        restored = restore_entry(*entry) && restored;
    }
    liveUnion = {};
    if (restored) {
        error.clear();
    } else {
        error = "未能验证据点资源联合已完整恢复。";
    }
    return restored;
}
}  // namespace base_resource_sharing::detail
