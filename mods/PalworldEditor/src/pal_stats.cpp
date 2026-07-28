/**
 * @file pal_stats.cpp
 * @brief 实现帕鲁属性编辑网关：导航 `SaveParameter` 结构体并读写等级/个体值/亲密度。
 * @details 所有接口在游戏线程执行，所有 Unreal 裸指针均为非拥有观察指针，
 *          不跨调用缓存任何句柄或属性指针。
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <game/pal_game.hpp>
#include <pal_stats/pal_stat_editor.hpp>
#include <pal_stats/pal_stats.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace pal_stats {
namespace {
/** @brief 把目标句柄还原为非拥有帕鲁 UObject，失效时返回 `nullptr`。 */
[[nodiscard]] auto to_pal(const PalStatTarget target) -> UObject* {
    auto* pal = reinterpret_cast<UObject*>(target);
    return pal_game::is_valid(pal) ? pal : nullptr;
}

/** @brief 调用无参、返回 `int32` 的 UFunction；目标或函数不可用时返回空。 */
[[nodiscard]] auto invoke_int_return(UObject* object, const TCHAR* name) -> std::optional<int> {
    if (!pal_game::is_valid(object)) {
        return std::nullopt;
    }
    auto* const function = object->GetFunctionByNameInChain(name);
    if (function == nullptr) {
        return std::nullopt;
    }
    struct Params {
        int32_t ReturnValue{};
    } params;
    object->ProcessEvent(function, &params);
    return params.ReturnValue;
}

/** @brief 取得帕鲁 `SaveParameter` 结构体内存指针与其 `FStructProperty`；不可达时返回 `nullptr`。
 */
[[nodiscard]] auto save_parameter_slot(UObject* pal, FStructProperty*& outProperty) -> void* {
    auto* const prop = pal->GetPropertyByNameInChain(STR("SaveParameter"));
    outProperty = CastField<FStructProperty>(prop);
    if (outProperty == nullptr || outProperty->GetStruct().Get() == nullptr) {
        return nullptr;
    }
    return outProperty->ContainerPtrToValuePtr<void>(pal);
}

/** @brief 查找 `SaveParameter` 中一个 `uint8` 字段；缺失或类型不匹配时返回空。 */
[[nodiscard]] auto find_byte(UStruct* rowStruct, const TCHAR* name) -> FByteProperty* {
    return rowStruct == nullptr
               ? nullptr
               : CastField<FByteProperty>(rowStruct->FindProperty(FName(name, FNAME_Find)));
}

/** @brief 读取已预检的 `uint8` 属性。 */
[[nodiscard]] auto read_byte(FByteProperty* property, const void* saveParam) -> int {
    return property->GetPropertyValueInContainer(saveParam);
}

/** @brief 把已裁剪的值写入预检通过的 `uint8` 属性。 */
auto write_byte(FByteProperty* property, void* saveParam, const int value) -> void {
    property->SetPropertyValueInContainer(saveParam, static_cast<std::uint8_t>(value));
}

/** @brief 取得 `PalDatabaseCharacterParameter` 单例；不可用时返回 `nullptr`。 */
[[nodiscard]] auto database() -> UObject* {
    return UObjectGlobals::FindFirstOf(STR("PalDatabaseCharacterParameter"));
}

/**
 * @brief 查询某亲密度 rank 所需的累计点数阈值。
 * @details 使用动态参数缓冲区匹配 `GetFriendshipRequiredPointByRank(int32, int32&)` 的 UFunction
 *          布局，避免手工对齐；查询失败时返回空，调用方必须保持零写入。
 */
[[nodiscard]] auto friendship_required_point(const int rank) -> std::optional<int> {
    auto* const db = database();
    auto* const function =
        db == nullptr ? nullptr
                      : db->GetFunctionByNameInChain(STR("GetFriendshipRequiredPointByRank"));
    if (function == nullptr) {
        return std::nullopt;
    }
    std::vector<std::byte> buffer(static_cast<std::size_t>(function->GetParmsSize()));
    function->InitializeStruct(buffer.data());
    struct DestroyGuard {
        UFunction* function{};
        void* params{};
        ~DestroyGuard() {
            function->DestroyStruct(params);
        }
    } guard{.function = function, .params = buffer.data()};

    auto* const rankProp =
        CastField<FIntProperty>(function->FindProperty(FName(STR("FriendshipRank"), FNAME_Find)));
    auto* const outProp =
        CastField<FIntProperty>(function->FindProperty(FName(STR("OutRequiredPoint"), FNAME_Find)));
    if (rankProp == nullptr || outProp == nullptr) {
        return std::nullopt;
    }
    rankProp->SetPropertyValueInContainer(buffer.data(), static_cast<int32_t>(rank));
    db->ProcessEvent(function, buffer.data());
    return outProp->GetPropertyValueInContainer(buffer.data());
}
}  // namespace

auto PalStatGateway::is_valid(const PalStatTarget target) const -> bool {
    return to_pal(target) != nullptr;
}

auto PalStatGateway::read_stats(const PalStatTarget target) -> PalStatSnapshot {
    PalStatSnapshot snapshot;
    auto* const pal = to_pal(target);
    if (pal == nullptr) {
        return snapshot;
    }
    const auto level = invoke_int_return(pal, STR("GetLevel"));
    const auto friendshipRank = invoke_int_return(pal, STR("GetFriendshipRank"));
    const auto friendshipPoint = invoke_int_return(pal, STR("GetFriendshipPoint"));
    if (!level.has_value() || !friendshipRank.has_value() || !friendshipPoint.has_value()) {
        return snapshot;
    }

    FStructProperty* saveProperty = nullptr;
    void* const saveParam = save_parameter_slot(pal, saveProperty);
    if (saveParam == nullptr) {
        return snapshot;
    }
    auto* const rowStruct = saveProperty->GetStruct().Get();
    auto* const talentHp = find_byte(rowStruct, STR("Talent_HP"));
    auto* const talentShot = find_byte(rowStruct, STR("Talent_Shot"));
    auto* const talentDefense = find_byte(rowStruct, STR("Talent_Defense"));
    if (talentHp == nullptr || talentShot == nullptr || talentDefense == nullptr) {
        return snapshot;
    }
    snapshot.level = *level;
    snapshot.friendshipRank = *friendshipRank;
    snapshot.friendshipPoint = *friendshipPoint;
    snapshot.talentHp = read_byte(talentHp, saveParam);
    snapshot.talentShot = read_byte(talentShot, saveParam);
    snapshot.talentDefense = read_byte(talentDefense, saveParam);
    snapshot.readable = true;
    return snapshot;
}

auto PalStatGateway::apply_stat_edit(const PalStatTarget target, const PalStatEditRequest& request)
    -> PalStatEditResult {
    PalStatEditResult result;
    auto* const pal = to_pal(target);
    if (pal == nullptr || !has_any_change(request.values)) {
        result.message = "属性修改已拒绝：目标无效或请求没有变化。";
        return result;
    }

    const auto before = read_stats(target);
    if (!before.readable) {
        result.status = PalStatEditStatus::preflightFailed;
        result.message = "属性修改未执行：无法完整读取修改前快照。";
        return result;
    }

    FStructProperty* saveProperty = nullptr;
    void* const saveParam = save_parameter_slot(pal, saveProperty);
    if (saveParam == nullptr) {
        result.status = PalStatEditStatus::preflightFailed;
        result.snapshot = before;
        result.message = "属性修改未执行：SaveParameter 结构不可用。";
        return result;
    }
    auto* const rowStruct = saveProperty->GetStruct().Get();

    auto* const level =
        request.values.level.has_value() ? find_byte(rowStruct, STR("Level")) : nullptr;
    auto* const talentHp =
        request.values.talentHp.has_value() ? find_byte(rowStruct, STR("Talent_HP")) : nullptr;
    auto* const talentShot =
        request.values.talentShot.has_value() ? find_byte(rowStruct, STR("Talent_Shot")) : nullptr;
    auto* const talentDefense = request.values.talentDefense.has_value()
                                    ? find_byte(rowStruct, STR("Talent_Defense"))
                                    : nullptr;
    auto* const friendshipPoint = request.values.friendshipRank.has_value()
                                      ? CastField<FIntProperty>(rowStruct->FindProperty(
                                            FName(STR("FriendshipPoint"), FNAME_Find)))
                                      : nullptr;
    const auto requiredFriendshipPoint =
        request.values.friendshipRank.has_value()
            ? friendship_required_point(clamp_friendship_rank(*request.values.friendshipRank))
            : std::optional<int>{};

    const bool preflightFailed =
        (request.values.level.has_value() && level == nullptr) ||
        (request.values.talentHp.has_value() && talentHp == nullptr) ||
        (request.values.talentShot.has_value() && talentShot == nullptr) ||
        (request.values.talentDefense.has_value() && talentDefense == nullptr) ||
        (request.values.friendshipRank.has_value() &&
         (friendshipPoint == nullptr || !requiredFriendshipPoint.has_value()));
    if (preflightFailed) {
        result.status = PalStatEditStatus::preflightFailed;
        result.snapshot = before;
        result.message = "属性修改未执行：当前游戏版本的字段或亲密度阈值不可用。";
        return result;
    }

    if (level != nullptr) {
        write_byte(level, saveParam, clamp_level(*request.values.level));
    }
    if (talentHp != nullptr) {
        write_byte(talentHp, saveParam, clamp_talent(*request.values.talentHp));
    }
    if (talentShot != nullptr) {
        write_byte(talentShot, saveParam, clamp_talent(*request.values.talentShot));
    }
    if (talentDefense != nullptr) {
        write_byte(talentDefense, saveParam, clamp_talent(*request.values.talentDefense));
    }
    if (friendshipPoint != nullptr) {
        friendshipPoint->SetPropertyValueInContainer(
            saveParam, static_cast<int32_t>(*requiredFriendshipPoint));
    }

    result.snapshot = read_stats(target);
    if (verify_stat_edit(request.values, result.snapshot)) {
        result.status = PalStatEditStatus::succeeded;
        result.message = "属性修改成功，并已通过游戏数据重读验证。";
        return result;
    }

    if (level != nullptr) {
        write_byte(level, saveParam, before.level);
    }
    if (talentHp != nullptr) {
        write_byte(talentHp, saveParam, before.talentHp);
    }
    if (talentShot != nullptr) {
        write_byte(talentShot, saveParam, before.talentShot);
    }
    if (talentDefense != nullptr) {
        write_byte(talentDefense, saveParam, before.talentDefense);
    }
    if (friendshipPoint != nullptr) {
        friendshipPoint->SetPropertyValueInContainer(saveParam,
                                                     static_cast<int32_t>(before.friendshipPoint));
    }

    result.snapshot = read_stats(target);
    PalStatValues rollbackExpected;
    if (request.values.level.has_value()) {
        rollbackExpected.level = before.level;
    }
    if (request.values.talentHp.has_value()) {
        rollbackExpected.talentHp = before.talentHp;
    }
    if (request.values.talentShot.has_value()) {
        rollbackExpected.talentShot = before.talentShot;
    }
    if (request.values.talentDefense.has_value()) {
        rollbackExpected.talentDefense = before.talentDefense;
    }
    if (request.values.friendshipRank.has_value()) {
        rollbackExpected.friendshipRank = before.friendshipRank;
    }
    if (verify_stat_edit(rollbackExpected, result.snapshot)) {
        result.status = PalStatEditStatus::verificationFailed;
        result.message = "属性写入后验证失败，已恢复修改前数值。";
    } else {
        result.status = PalStatEditStatus::rollbackFailed;
        result.message = "属性写入和恢复验证均失败；请立即退出当前世界并检查存档。";
    }
    return result;
}
}  // namespace pal_stats
