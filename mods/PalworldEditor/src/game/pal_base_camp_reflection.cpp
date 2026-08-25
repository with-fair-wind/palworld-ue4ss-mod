/**
 * @file pal_base_camp_reflection.cpp
 * @brief 实现基地管理器与地图物体管理器的经签名校验反射调用。
 */
#include <cstddef>

#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/game_reflection.hpp>
#include <game/pal_base_camp_reflection.hpp>

using namespace RC::Unreal;

namespace pal_base_camp_reflection {
namespace {

[[nodiscard]] auto guid_is_nonzero(const FGuid& guid) -> bool {
    return guid.A != 0 || guid.B != 0 || guid.C != 0 || guid.D != 0;
}

[[nodiscard]] auto find_pal_utility() -> UObject* {
    return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr,
                                                      STR("/Script/Pal.Default__PalUtility"));
}

/** @brief 调用目标对象上返回 FGuid 的无参 getter；返回值全零视为失败。 */
[[nodiscard]] auto call_guid_getter(UObject* target, const RC::CharType* functionName,
                                    FGuid& output) -> bool {
    auto* const function =
        pal_game::is_valid(target) ? target->GetFunctionByNameInChain(functionName) : nullptr;
    auto* const returnProperty =
        function == nullptr ? nullptr : CastField<FStructProperty>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_return_parameter(returnProperty) ||
        !pal_game::matches_struct_identity(returnProperty, STR("Guid"), sizeof(FGuid))) {
        return false;
    }
    pal_game::FunctionParams params{function};
    target->ProcessEvent(function, params.data());
    returnProperty->CopyCompleteValue(&output,
                                      returnProperty->ContainerPtrToValuePtr<void>(params.data()));
    return guid_is_nonzero(output);
}

/** @brief 调用 PalUtility:GetGuildByPlayerUId。 */
[[nodiscard]] auto try_get_player_guild(UObject* worldContext, const FGuid& playerId,
                                        UObject*& guild) -> bool {
    guild = nullptr;
    auto* utility = find_pal_utility();
    auto* function = pal_game::is_valid(utility)
                         ? utility->GetFunctionByNameInChain(STR("GetGuildByPlayerUId"))
                         : nullptr;
    auto* const context = function == nullptr
                              ? nullptr
                              : CastField<FObjectPropertyBase>(function->FindProperty(
                                    FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const player = function == nullptr ? nullptr
                                             : CastField<FStructProperty>(function->FindProperty(
                                                   FName(STR("PlayerUId"), FNAME_Find)));
    auto* const result = function == nullptr
                             ? nullptr
                             : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 3) ||
        !pal_game::is_input_parameter(context) || !pal_game::is_input_parameter(player) ||
        !pal_game::matches_struct_identity(player, STR("Guid"), sizeof(FGuid)) ||
        !pal_game::is_return_parameter(result)) {
        return false;
    }
    pal_game::FunctionParams params{function};
    context->SetObjectPropertyValue(context->ContainerPtrToValuePtr<void>(params.data()),
                                    worldContext);
    player->CopyCompleteValue(player->ContainerPtrToValuePtr<void>(params.data()), &playerId);
    utility->ProcessEvent(function, params.data());
    guild = result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(params.data()));
    return pal_game::is_valid(guild);
}
constexpr int32 kMaximumBaseCampCount = 1'024;
}  // namespace

auto read_base_ids(UObject* manager, std::vector<FGuid>& output) -> bool {
    output.clear();
    auto* const function = pal_game::is_valid(manager)
                               ? manager->GetFunctionByNameInChain(STR("GetBaseCampIds"))
                               : nullptr;
    auto* const arrayProperty =
        function == nullptr
            ? nullptr
            : CastField<FArrayProperty>(function->FindProperty(FName(STR("OutIds"), FNAME_Find)));
    auto* const guidProperty =
        arrayProperty == nullptr ? nullptr : CastField<FStructProperty>(arrayProperty->GetInner());
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_output_parameter(arrayProperty) ||
        !pal_game::matches_struct_identity(guidProperty, STR("Guid"), sizeof(FGuid))) {
        return false;
    }

    pal_game::FunctionParams params{function};
    manager->ProcessEvent(function, params.data());
    FScriptArrayHelper_InContainer values(arrayProperty, params.data());
    const int32 count = values.Num();
    if (count < 0 || count > kMaximumBaseCampCount) {
        return false;
    }
    output.reserve(static_cast<std::size_t>(count));
    for (int32 index{}; index < count; ++index) {
        FGuid id{};
        guidProperty->CopyCompleteValue(&id, values.GetRawPtr(index));
        output.push_back(id);
    }
    return true;
}

auto try_get_base_model(UObject* manager, const FGuid& baseId, UObject*& model) -> bool {
    model = nullptr;
    auto* const function = pal_game::is_valid(manager)
                               ? manager->GetFunctionByNameInChain(STR("TryGetModel"))
                               : nullptr;
    auto* const idProperty =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("BaseCampId"), FNAME_Find)));
    auto* const modelProperty =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(
                                  function->FindProperty(FName(STR("OutModel"), FNAME_Find)));
    auto* const returnProperty =
        function == nullptr ? nullptr : CastField<FBoolProperty>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 3) ||
        !pal_game::is_input_parameter(idProperty) ||
        !pal_game::matches_struct_identity(idProperty, STR("Guid"), sizeof(FGuid)) ||
        !pal_game::is_output_parameter(modelProperty) ||
        !pal_game::is_return_parameter(returnProperty)) {
        return false;
    }

    pal_game::FunctionParams params{function};
    idProperty->CopyCompleteValue(idProperty->ContainerPtrToValuePtr<void>(params.data()), &baseId);
    manager->ProcessEvent(function, params.data());
    auto* const result = modelProperty->GetObjectPropertyValue(
        modelProperty->ContainerPtrToValuePtr<void>(params.data()));
    if (returnProperty->GetPropertyValueInContainer(params.data()) && pal_game::is_valid(result)) {
        model = result;
    }
    return model != nullptr;
}

auto find_concrete_model(UObject* manager, const FGuid& instanceId, UObject*& concreteModel)
    -> bool {
    concreteModel = nullptr;
    auto* const function = pal_game::is_valid(manager)
                               ? manager->GetFunctionByNameInChain(STR("FindConcreteModel"))
                               : nullptr;
    auto* const inputProperty =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("InstanceId"), FNAME_Find)));
    auto* const returnProperty =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 2) ||
        !pal_game::is_input_parameter(inputProperty) ||
        !pal_game::matches_struct_identity(inputProperty, STR("Guid"), sizeof(FGuid)) ||
        !pal_game::is_return_parameter(returnProperty)) {
        return false;
    }

    pal_game::FunctionParams params{function};
    inputProperty->CopyCompleteValue(inputProperty->ContainerPtrToValuePtr<void>(params.data()),
                                     &instanceId);
    manager->ProcessEvent(function, params.data());
    auto* const result = returnProperty->GetObjectPropertyValue(
        returnProperty->ContainerPtrToValuePtr<void>(params.data()));
    concreteModel = pal_game::is_valid(result) ? result : nullptr;
    return concreteModel != nullptr;
}

auto resolve_local_guild_id(UObject* worldContext, FGuid& guildId) -> bool {
    guildId = FGuid{};
    auto* utility = find_pal_utility();
    auto* const controllerFunction =
        pal_game::is_valid(utility)
            ? utility->GetFunctionByNameInChain(STR("GetLocalPalPlayerController"))
            : nullptr;
    auto* const controllerContext =
        controllerFunction == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(
                  controllerFunction->FindProperty(FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const controllerReturn =
        controllerFunction == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(controllerFunction->GetReturnProperty());
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(controllerFunction, 2) ||
        !pal_game::is_input_parameter(controllerContext) ||
        !pal_game::is_return_parameter(controllerReturn)) {
        return false;
    }
    pal_game::FunctionParams controllerParams{controllerFunction};
    controllerContext->SetObjectPropertyValue(
        controllerContext->ContainerPtrToValuePtr<void>(controllerParams.data()), worldContext);
    utility->ProcessEvent(controllerFunction, controllerParams.data());
    auto* const controller = controllerReturn->GetObjectPropertyValue(
        controllerReturn->ContainerPtrToValuePtr<void>(controllerParams.data()));
    if (!pal_game::is_valid(controller)) {
        return false;
    }
    FGuid playerId{};
    UObject* guild{};
    // 玩家 UId 与公会查询都以 controller 为上下文/目标（与资源共享原实现语义一致）。
    return call_guid_getter(controller, STR("GetPlayerUId"), playerId) &&
           try_get_player_guild(controller, playerId, guild) &&
           call_guid_getter(guild, STR("GetId"), guildId);
}
}  // namespace pal_base_camp_reflection
