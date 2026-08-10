/**
 * @file pal_identity.cpp
 * @brief 实现 Alpha、Lucky 与觉醒的游戏线程反射事务。
 * @details 不缓存 UObject 或属性地址；只有显式选择/应用请求会进入本文件。
 */
#include <optional>
#include <string>
#include <string_view>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/text_encoding.hpp>
#include <game/pal_game.hpp>
#include <pal_identity/pal_identity.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace pal_identity {
using pal_game::FunctionParams;
namespace {
inline constexpr std::string_view kBossPrefix{"BOSS_"};

struct SaveFields {
    void* saveParameter{};
    UStruct* saveStruct{};
    FNameProperty* characterId{};
    FBoolProperty* rare{};
    FBoolProperty* awakening{};
};

[[nodiscard]] auto to_object(const std::uintptr_t value) -> UObject* {
    auto* const object = reinterpret_cast<UObject*>(value);
    return pal_game::is_valid(object) ? object : nullptr;
}

[[nodiscard]] auto prepare_save_fields(UObject* pal) -> std::optional<SaveFields> {
    if (!pal_game::is_valid(pal)) {
        return std::nullopt;
    }
    auto* const saveProperty =
        CastField<FStructProperty>(pal->GetPropertyByNameInChain(STR("SaveParameter")));
    auto* const saveStruct = saveProperty == nullptr ? nullptr : saveProperty->GetStruct().Get();
    void* const saveParameter =
        saveProperty == nullptr ? nullptr : saveProperty->ContainerPtrToValuePtr<void>(pal);
    auto* const characterId = saveStruct == nullptr
                                  ? nullptr
                                  : CastField<FNameProperty>(saveStruct->FindProperty(
                                        FName(STR("CharacterID"), FNAME_Find)));
    auto* const rare = saveStruct == nullptr ? nullptr
                                             : CastField<FBoolProperty>(saveStruct->FindProperty(
                                                   FName(STR("IsRarePal"), FNAME_Find)));
    auto* const awakening = saveStruct == nullptr
                                ? nullptr
                                : CastField<FBoolProperty>(saveStruct->FindProperty(
                                      FName(STR("bIsAwakening"), FNAME_Find)));
    if (saveParameter == nullptr || saveStruct == nullptr || characterId == nullptr ||
        rare == nullptr || awakening == nullptr) {
        return std::nullopt;
    }
    return SaveFields{
        .saveParameter = saveParameter,
        .saveStruct = saveStruct,
        .characterId = characterId,
        .rare = rare,
        .awakening = awakening,
    };
}

[[nodiscard]] auto database_for(UObject* worldContext) -> UObject* {
    auto* const utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* const function =
        utility == nullptr
            ? nullptr
            : utility->GetFunctionByNameInChain(STR("GetDatabaseCharacterParameter"));
    auto* const input = function == nullptr ? nullptr
                                            : CastField<FObjectPropertyBase>(function->FindProperty(
                                                  FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const output = function == nullptr
                             ? nullptr
                             : CastField<FObjectPropertyBase>(
                                   function->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 2) || !pal_game::is_input_parameter(input) ||
        !pal_game::is_return_parameter(output)) {
        return nullptr;
    }
    FunctionParams params{function};
    input->SetObjectPropertyValue(input->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    utility->ProcessEvent(function, params.data());
    auto* const database =
        output->GetObjectPropertyValue(output->ContainerPtrToValuePtr<void>(params.data()));
    return pal_game::is_valid(database) ? database : nullptr;
}

[[nodiscard]] auto database_bool(UObject* database, const TCHAR* functionName,
                                 const FName characterId) -> std::optional<bool> {
    auto* const function =
        pal_game::is_valid(database) ? database->GetFunctionByNameInChain(functionName) : nullptr;
    auto* const input =
        function == nullptr
            ? nullptr
            : CastField<FNameProperty>(function->FindProperty(FName(STR("RowName"), FNAME_Find)));
    auto* const output = function == nullptr ? nullptr
                                             : CastField<FBoolProperty>(function->FindProperty(
                                                   FName(STR("ReturnValue"), FNAME_Find)));
    if (!pal_game::has_exact_parameter_count(function, 2) || !pal_game::is_input_parameter(input) ||
        !pal_game::is_return_parameter(output)) {
        return std::nullopt;
    }
    FunctionParams params{function};
    input->SetPropertyValueInContainer(params.data(), characterId);
    database->ProcessEvent(function, params.data());
    return output->GetPropertyValueInContainer(params.data());
}

[[nodiscard]] auto update_database_parameter(UObject* database, UObject* pal) -> bool {
    auto* const function =
        pal_game::is_valid(database)
            ? database->GetFunctionByNameInChain(STR("UpdateApplyDatabaseToIndividualParameter"))
            : nullptr;
    auto* const input = function == nullptr ? nullptr
                                            : CastField<FObjectPropertyBase>(function->FindProperty(
                                                  FName(STR("IndividualParameter"), FNAME_Find)));
    if (!pal_game::is_valid(pal) || !pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_input_parameter(input) || function->GetReturnProperty() != nullptr) {
        return false;
    }
    FunctionParams params{function};
    input->SetObjectPropertyValue(input->ContainerPtrToValuePtr<void>(params.data()), pal);
    database->ProcessEvent(function, params.data());
    return true;
}

[[nodiscard]] auto notify_save_parameter_changed(UObject* pal) -> bool {
    auto* const function = pal_game::is_valid(pal)
                               ? pal->GetFunctionByNameInChain(STR("OnRep_SaveParameter"))
                               : nullptr;
    if (!pal_game::has_exact_parameter_count(function, 0) ||
        function->GetReturnProperty() != nullptr) {
        return false;
    }
    pal->ProcessEvent(function, nullptr);
    return true;
}

struct AlphaPair {
    std::string baseId;
    std::string alphaId;
};

[[nodiscard]] auto to_name(const std::string_view id) -> FName {
    const auto wide = text_encoding::widen_ascii(id);
    return FName(wide.c_str());
}

[[nodiscard]] auto is_special_boss(UObject* database, const FName id) -> std::optional<bool> {
    const auto tower = database_bool(database, STR("GetIsTowerBoss"), id);
    const auto raid = database_bool(database, STR("GetIsRaidBoss"), id);
    const auto predator = database_bool(database, STR("GetIsPredatorBoss"), id);
    if (!tower.has_value() || !raid.has_value() || !predator.has_value()) {
        return std::nullopt;
    }
    return *tower || *raid || *predator;
}

[[nodiscard]] auto resolve_alpha_pair(UObject* database, const std::string_view currentId,
                                      const bool currentIsBoss) -> std::optional<AlphaPair> {
    std::string baseId;
    std::string alphaId;
    if (currentId.starts_with(kBossPrefix)) {
        baseId = std::string{currentId.substr(kBossPrefix.size())};
        alphaId = std::string{currentId};
    } else {
        baseId = std::string{currentId};
        alphaId = std::string{kBossPrefix};
        alphaId.append(currentId);
    }
    if (baseId.empty() || alphaId.size() <= kBossPrefix.size()) {
        return std::nullopt;
    }
    const auto baseBoss = database_bool(database, STR("GetIsBoss"), to_name(baseId));
    const auto alphaBoss = database_bool(database, STR("GetIsBoss"), to_name(alphaId));
    const auto special = is_special_boss(database, to_name(alphaId));
    if (!baseBoss.has_value() || !alphaBoss.has_value() || !special.has_value() || *baseBoss ||
        !*alphaBoss || *special) {
        return std::nullopt;
    }
    const bool currentMatchesPair = currentId == baseId || currentId == alphaId;
    const bool currentClassificationMatches = currentIsBoss == (currentId == alphaId);
    if (!currentMatchesPair || !currentClassificationMatches) {
        return std::nullopt;
    }
    return AlphaPair{.baseId = std::move(baseId), .alphaId = std::move(alphaId)};
}

auto write_identity(const SaveFields& fields, const FName characterId, const bool rare,
                    const bool awakening) -> void {
    fields.characterId->SetPropertyValueInContainer(fields.saveParameter, characterId);
    fields.rare->SetPropertyValueInContainer(fields.saveParameter, rare);
    fields.awakening->SetPropertyValueInContainer(fields.saveParameter, awakening);
}

[[nodiscard]] auto refresh_identity(UObject* database, UObject* pal) -> bool {
    return update_database_parameter(database, pal) && notify_save_parameter_changed(pal);
}
}  // namespace

auto PalIdentityGateway::read_identity(const PalIdentityTarget target, const bool spawnStateKnown,
                                       const bool selectedIsSpawned) -> PalIdentitySnapshot {
    PalIdentitySnapshot snapshot;
    auto* const pal = to_object(target);
    const auto fields = prepare_save_fields(pal);
    auto* const database = database_for(pal);
    if (!fields.has_value() || database == nullptr || !spawnStateKnown) {
        return snapshot;
    }

    const FName rawCharacterId =
        fields->characterId->GetPropertyValueInContainer(fields->saveParameter);
    const bool rawRare = fields->rare->GetPropertyValueInContainer(fields->saveParameter);
    const bool rawAwakening = fields->awakening->GetPropertyValueInContainer(fields->saveParameter);
    const auto getterCharacterId = pal_game::invoke<FName>(pal, STR("GetCharacterID"));
    const auto getterRare = pal_game::invoke<bool>(pal, STR("IsRarePal"));
    const auto getterAwakening = pal_game::invoke<bool>(pal, STR("IsAwakening"));
    const auto currentBoss = database_bool(database, STR("GetIsBoss"), rawCharacterId);
    if (!getterCharacterId.has_value() || !getterRare.has_value() || !getterAwakening.has_value() ||
        !currentBoss.has_value() || *getterCharacterId != rawCharacterId ||
        *getterRare != rawRare || *getterAwakening != rawAwakening) {
        return snapshot;
    }

    snapshot.characterId = text_encoding::to_utf8(rawCharacterId.ToString());
    snapshot.alpha = *currentBoss;
    snapshot.lucky = rawRare;
    snapshot.awakening = rawAwakening;
    snapshot.spawnStateKnown = true;
    snapshot.summoned = selectedIsSpawned;
    if (const auto pair = resolve_alpha_pair(database, snapshot.characterId, snapshot.alpha);
        pair.has_value()) {
        snapshot.baseCharacterId = pair->baseId;
        snapshot.alphaCharacterId = pair->alphaId;
        snapshot.alphaAvailable = true;
    }
    snapshot.readable = !snapshot.characterId.empty();
    return snapshot;
}

auto PalIdentityGateway::apply_identity_edit(const PalIdentityTarget target,
                                             const bool spawnStateKnown,
                                             const bool selectedIsSpawned,
                                             const PalIdentityEditRequest& request)
    -> PalIdentityEditResult {
    PalIdentityEditResult result;
    auto* const pal = to_object(target);
    if (pal == nullptr || !spawnStateKnown || !has_any_change(request.values)) {
        result.message = "形态修改已拒绝：目标无效或请求没有变化。";
        return result;
    }

    const auto before = read_identity(target, spawnStateKnown, selectedIsSpawned);
    result.snapshot = before;
    if (!before.readable) {
        result.status = PalIdentityEditStatus::preflightFailed;
        result.message = "形态修改未执行：无法完整读取 CharacterID、Lucky 或觉醒状态。";
        return result;
    }
    if (before.summoned) {
        result.status = PalIdentityEditStatus::rejected;
        result.message = "形态修改未执行：请先按 E 收回当前帕鲁，再点击应用。";
        return result;
    }
    if (request.values.alpha.has_value() && !before.alphaAvailable) {
        result.status = PalIdentityEditStatus::preflightFailed;
        result.message = "Alpha 修改未执行：当前物种没有通过原生数据库验证的普通/BOSS 配对。";
        return result;
    }

    const auto fields = prepare_save_fields(pal);
    auto* const database = database_for(pal);
    auto* const onRep = pal_game::is_valid(pal)
                            ? pal->GetFunctionByNameInChain(STR("OnRep_SaveParameter"))
                            : nullptr;
    auto* const update =
        pal_game::is_valid(database)
            ? database->GetFunctionByNameInChain(STR("UpdateApplyDatabaseToIndividualParameter"))
            : nullptr;
    if (!fields.has_value() || database == nullptr || onRep == nullptr ||
        onRep->GetParmsSize() != 0 || update == nullptr) {
        result.status = PalIdentityEditStatus::preflightFailed;
        result.message = "形态修改未执行：当前游戏版本的存档字段或原生刷新接口不可用。";
        return result;
    }

    const std::string desiredCharacterId =
        request.values.alpha.has_value()
            ? (*request.values.alpha ? before.alphaCharacterId : before.baseCharacterId)
            : before.characterId;
    const bool desiredRare = request.values.lucky.value_or(before.lucky);
    const bool desiredAwakening = request.values.awakening.value_or(before.awakening);
    const FName beforeCharacterId = to_name(before.characterId);
    const FName desiredCharacterName = to_name(desiredCharacterId);

    write_identity(*fields, desiredCharacterName, desiredRare, desiredAwakening);
    const bool writesCompleted = refresh_identity(database, pal);
    result.snapshot = read_identity(target, spawnStateKnown, selectedIsSpawned);
    const bool characterIdMatches =
        !request.values.alpha.has_value() || result.snapshot.characterId == desiredCharacterId;
    if (writesCompleted && characterIdMatches &&
        verify_identity_edit(request.values, result.snapshot)) {
        result.status = PalIdentityEditStatus::succeeded;
        result.message = "Alpha、Lucky 与觉醒状态修改成功，并已通过原生接口重读验证。";
        return result;
    }

    write_identity(*fields, beforeCharacterId, before.lucky, before.awakening);
    const bool rollbackOperationsSucceeded = refresh_identity(database, pal);
    result.snapshot = read_identity(target, spawnStateKnown, selectedIsSpawned);
    const PalIdentityValues rollbackExpected{
        .alpha = before.alpha,
        .lucky = before.lucky,
        .awakening = before.awakening,
    };
    const bool characterIdRestored = result.snapshot.characterId == before.characterId;
    if (rollbackOperationsSucceeded && characterIdRestored &&
        verify_identity_edit(rollbackExpected, result.snapshot)) {
        result.status = PalIdentityEditStatus::verificationFailed;
        result.message = "形态写入后验证失败，已恢复修改前状态。";
    } else {
        result.status = PalIdentityEditStatus::rollbackFailed;
        result.message = "形态写入和恢复验证均失败；已停用本世界形态写入，请立即退出世界检查存档。";
    }
    return result;
}
}  // namespace pal_identity
