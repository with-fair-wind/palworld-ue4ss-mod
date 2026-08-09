/**
 * @file pal_revive.cpp
 * @brief 实现队伍帕鲁复活的反射预检、写后校验与失败回滚。
 */
#include <cstdint>
#include <optional>
#include <utility>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UObject.hpp>
#include <game/pal_game.hpp>
#include <pal_revive/pal_revive.hpp>

using namespace RC::Unreal;

namespace pal_revive {
namespace {
struct ReviveAccess {
    void* saveParameter{};
    FEnumProperty* physicalHealth{};
    FNumericProperty* physicalHealthUnderlying{};
    void* hp{};
    FInt64Property* hpValue{};
    UFunction* setPhysicalHealth{};
    FEnumProperty* setPhysicalHealthInput{};
    FNumericProperty* setPhysicalHealthInputUnderlying{};
    UFunction* fullRecoveryHp{};
    UFunction* onRepSaveParameter{};
};

struct ReviveState {
    std::int64_t physicalHealth{};
    std::int64_t hpValue{};
};

enum class ReviveAttemptStatus : std::uint8_t {
    succeeded,
    preflightFailed,
    rolledBack,
    rollbackFailed,
};

[[nodiscard]] auto read_no_param_int(UObject* object, const TCHAR* functionName)
    -> std::optional<int32> {
    auto* const function = pal_game::is_valid(object)
                               ? object->GetFunctionByNameInChain(functionName)
                               : nullptr;
    auto* const returnProperty =
        function == nullptr ? nullptr : CastField<FIntProperty>(function->GetReturnProperty());
    if (returnProperty == nullptr ||
        std::cmp_not_equal(function->GetParmsSize(), returnProperty->GetElementSize()) ||
        !returnProperty->HasAnyPropertyFlags(CPF_Parm) ||
        !returnProperty->HasAnyPropertyFlags(CPF_ReturnParm)) {
        return std::nullopt;
    }

    pal_game::FunctionParams params{function};
    object->ProcessEvent(function, params.data());
    return returnProperty->GetPropertyValueInContainer(params.data());
}

[[nodiscard]] auto read_no_param_object(UObject* object, const TCHAR* functionName)
    -> UObject* {
    auto* const function = pal_game::is_valid(object)
                               ? object->GetFunctionByNameInChain(functionName)
                               : nullptr;
    auto* const returnProperty = function == nullptr
                                     ? nullptr
                                     : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (returnProperty == nullptr ||
        std::cmp_not_equal(function->GetParmsSize(), returnProperty->GetElementSize()) ||
        !returnProperty->HasAnyPropertyFlags(CPF_Parm) ||
        !returnProperty->HasAnyPropertyFlags(CPF_ReturnParm)) {
        return nullptr;
    }

    pal_game::FunctionParams params{function};
    object->ProcessEvent(function, params.data());
    auto* const result = returnProperty->GetObjectPropertyValue(
        returnProperty->ContainerPtrToValuePtr<void>(params.data()));
    return pal_game::is_valid(result) ? result : nullptr;
}

[[nodiscard]] auto prepare_revive_access(UObject* parameter) -> std::optional<ReviveAccess> {
    if (!pal_game::is_valid(parameter)) {
        return std::nullopt;
    }

    auto* const saveProperty =
        CastField<FStructProperty>(parameter->GetPropertyByNameInChain(STR("SaveParameter")));
    auto* const saveStruct = saveProperty == nullptr ? nullptr : saveProperty->GetStruct().Get();
    void* const saveParameter =
        saveProperty == nullptr ? nullptr : saveProperty->ContainerPtrToValuePtr<void>(parameter);
    auto* const physicalHealth =
        saveStruct == nullptr
            ? nullptr
            : CastField<FEnumProperty>(
                  saveStruct->FindProperty(FName(STR("PhysicalHealth"), FNAME_Find)));
    auto* const physicalHealthUnderlying =
        physicalHealth == nullptr ? nullptr : physicalHealth->GetUnderlyingProperty();
    auto* const hpProperty =
        saveStruct == nullptr
            ? nullptr
            : CastField<FStructProperty>(saveStruct->FindProperty(FName(STR("Hp"), FNAME_Find)));
    auto* const hpStruct = hpProperty == nullptr ? nullptr : hpProperty->GetStruct().Get();
    void* const hp = hpProperty == nullptr || saveParameter == nullptr
                         ? nullptr
                         : hpProperty->ContainerPtrToValuePtr<void>(saveParameter);
    auto* const hpValue =
        hpStruct == nullptr
            ? nullptr
            : CastField<FInt64Property>(hpStruct->FindProperty(FName(STR("Value"), FNAME_Find)));

    auto* const setPhysicalHealth = parameter->GetFunctionByNameInChain(STR("SetPhysicalHealth"));
    auto* const setPhysicalHealthInput =
        setPhysicalHealth == nullptr
            ? nullptr
            : CastField<FEnumProperty>(
                  setPhysicalHealth->FindProperty(FName(STR("PhysicalHealth"), FNAME_Find)));
    auto* const setPhysicalHealthInputUnderlying =
        setPhysicalHealthInput == nullptr ? nullptr
                                          : setPhysicalHealthInput->GetUnderlyingProperty();
    auto* const fullRecoveryHp = parameter->GetFunctionByNameInChain(STR("FullRecoveryHP"));
    auto* const onRepSaveParameter =
        parameter->GetFunctionByNameInChain(STR("OnRep_SaveParameter"));

    if (saveParameter == nullptr || physicalHealth == nullptr ||
        physicalHealthUnderlying == nullptr || !physicalHealthUnderlying->IsInteger() ||
        hp == nullptr || hpValue == nullptr || setPhysicalHealth == nullptr ||
        setPhysicalHealthInput == nullptr || setPhysicalHealthInputUnderlying == nullptr ||
        !setPhysicalHealthInputUnderlying->IsInteger() ||
        !setPhysicalHealthInput->HasAnyPropertyFlags(CPF_Parm) ||
        setPhysicalHealthInput->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm) ||
        fullRecoveryHp == nullptr || fullRecoveryHp->GetParmsSize() != 0 ||
        onRepSaveParameter == nullptr || onRepSaveParameter->GetParmsSize() != 0) {
        return std::nullopt;
    }

    return ReviveAccess{
        .saveParameter = saveParameter,
        .physicalHealth = physicalHealth,
        .physicalHealthUnderlying = physicalHealthUnderlying,
        .hp = hp,
        .hpValue = hpValue,
        .setPhysicalHealth = setPhysicalHealth,
        .setPhysicalHealthInput = setPhysicalHealthInput,
        .setPhysicalHealthInputUnderlying = setPhysicalHealthInputUnderlying,
        .fullRecoveryHp = fullRecoveryHp,
        .onRepSaveParameter = onRepSaveParameter,
    };
}

[[nodiscard]] auto read_revive_state(const ReviveAccess& access) -> ReviveState {
    return ReviveState{
        .physicalHealth = access.physicalHealthUnderlying->GetSignedIntPropertyValue(
            access.physicalHealth->ContainerPtrToValuePtr<void>(access.saveParameter)),
        .hpValue = access.hpValue->GetPropertyValueInContainer(access.hp),
    };
}

auto set_physical_health(UObject* parameter, const ReviveAccess& access,
                         const std::int64_t value) -> void {
    pal_game::FunctionParams params{access.setPhysicalHealth};
    access.setPhysicalHealthInputUnderlying->SetIntPropertyValue(
        access.setPhysicalHealthInput->ContainerPtrToValuePtr<void>(params.data()), value);
    parameter->ProcessEvent(access.setPhysicalHealth, params.data());
}

[[nodiscard]] auto restore_revive_state(UObject* parameter, const ReviveState& original) -> bool {
    auto access = prepare_revive_access(parameter);
    if (!access.has_value()) {
        return false;
    }
    set_physical_health(parameter, *access, original.physicalHealth);

    access = prepare_revive_access(parameter);
    if (!access.has_value()) {
        return false;
    }
    access->hpValue->SetPropertyValueInContainer(access->hp, original.hpValue);
    parameter->ProcessEvent(access->onRepSaveParameter, nullptr);

    access = prepare_revive_access(parameter);
    if (!access.has_value()) {
        return false;
    }
    const auto restored = read_revive_state(*access);
    return restored.physicalHealth == original.physicalHealth &&
           restored.hpValue == original.hpValue;
}

[[nodiscard]] auto apply_revive(UObject* parameter, const ReviveState& original)
    -> ReviveAttemptStatus {
    auto access = prepare_revive_access(parameter);
    if (!access.has_value()) {
        return ReviveAttemptStatus::preflightFailed;
    }
    set_physical_health(parameter, *access, 0);
    const auto rollback = [&] {
        return restore_revive_state(parameter, original) ? ReviveAttemptStatus::rolledBack
                                                         : ReviveAttemptStatus::rollbackFailed;
    };

    access = prepare_revive_access(parameter);
    if (!access.has_value()) {
        return rollback();
    }
    parameter->ProcessEvent(access->fullRecoveryHp, nullptr);

    access = prepare_revive_access(parameter);
    if (!access.has_value()) {
        return rollback();
    }
    parameter->ProcessEvent(access->onRepSaveParameter, nullptr);

    access = prepare_revive_access(parameter);
    if (!access.has_value()) {
        return rollback();
    }
    const auto after = read_revive_state(*access);
    return after.physicalHealth == 0 && after.hpValue > 0 ? ReviveAttemptStatus::succeeded
                                                          : rollback();
}
}  // namespace

auto revive_team_pals() -> TeamReviveResult {
    TeamReviveResult result;
    auto* const holder = pal_game::resolve_local_otomo_holder().holder;
    if (!pal_game::is_valid(holder)) {
        result.error = TeamReviveError::holderUnavailable;
        return result;
    }

    const auto maxNum = read_no_param_int(holder, STR("GetMaxOtomoNum")).value_or(0);
    if (maxNum <= 0 || maxNum > 20) {
        result.error = TeamReviveError::invalidSlotCount;
        return result;
    }

    for (int32 slotIndex = 0; slotIndex < maxNum; ++slotIndex) {
        UObject* handle{};
        if (!pal_game::try_get_otomo_individual_handle(holder, slotIndex, handle)) {
            result.error = TeamReviveError::handleInterfaceUnavailable;
            return result;
        }
        if (!pal_game::is_valid(handle)) {
            continue;
        }

        auto* const parameter = read_no_param_object(handle, STR("TryGetIndividualParameter"));
        if (!pal_game::is_valid(parameter)) {
            continue;
        }

        const auto access = prepare_revive_access(parameter);
        if (!access.has_value()) {
            ++result.failedCount;
            continue;
        }
        const auto original = read_revive_state(*access);
        if (original.physicalHealth <= 0) {
            continue;
        }

        switch (apply_revive(parameter, original)) {
            case ReviveAttemptStatus::succeeded:
                ++result.revivedCount;
                break;
            case ReviveAttemptStatus::preflightFailed:
            case ReviveAttemptStatus::rolledBack:
                ++result.failedCount;
                break;
            case ReviveAttemptStatus::rollbackFailed:
                ++result.failedCount;
                result.rollbackFailed = true;
                return result;
        }
    }
    return result;
}
}  // namespace pal_revive
