/**
 * @file pal_stats.cpp
 * @brief 实现帕鲁属性编辑网关：导航 `SaveParameter` 结构体并读写等级/个体值/亲密度。
 * @details 所有接口在游戏线程执行，所有 Unreal 裸指针均为非拥有观察指针，
 *          不跨调用缓存任何句柄或属性指针。
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
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
/** @brief 亲密度阈值查询失败时的保守回退点数（rank 0 对应 0 点）。 */
inline constexpr int kFriendshipPointFallback = 0;

/** @brief 把目标句柄还原为非拥有帕鲁 UObject，失效时返回 `nullptr`。 */
[[nodiscard]] auto to_pal(const PalStatTarget target) -> UObject* {
    auto* pal = reinterpret_cast<UObject*>(target);
    return pal_game::is_valid(pal) ? pal : nullptr;
}

/** @brief 调用无参、返回 `int32` 的 UFunction；目标/函数不可用时返回 `fallback`。 */
[[nodiscard]] auto invoke_int_return(UObject* object, const TCHAR* name, const int fallback = 0)
    -> int {
    if (!pal_game::is_valid(object)) {
        return fallback;
    }
    auto* const function = object->GetFunctionByNameInChain(name);
    if (function == nullptr) {
        return fallback;
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

/** @brief 读取 `SaveParameter` 中一个 `uint8` 字段；缺失时返回 `fallback`。 */
[[nodiscard]] auto read_byte(UStruct* rowStruct, void* saveParam, const TCHAR* name,
                             const int fallback = 0) -> int {
    auto* const prop = CastField<FByteProperty>(rowStruct->FindProperty(FName(name, FNAME_Find)));
    if (prop == nullptr) {
        return fallback;
    }
    return prop->GetPropertyValueInContainer(saveParam);
}

/** @brief 把 `value` 经 `clampFn` 裁剪后写入 `SaveParameter` 中一个 `uint8` 字段。 */
auto write_byte(UStruct* rowStruct, void* saveParam, const TCHAR* name, const int value,
                int (*clampFn)(int)) -> void {
    auto* const prop = CastField<FByteProperty>(rowStruct->FindProperty(FName(name, FNAME_Find)));
    if (prop != nullptr) {
        prop->SetPropertyValueInContainer(saveParam, static_cast<std::uint8_t>(clampFn(value)));
    }
}

/** @brief 取得 `PalDatabaseCharacterParameter` 单例；不可用时返回 `nullptr`。 */
[[nodiscard]] auto database() -> UObject* {
    return UObjectGlobals::FindFirstOf(STR("PalDatabaseCharacterParameter"));
}

/**
 * @brief 查询某亲密度 rank 所需的累计点数阈值。
 * @details 使用动态参数缓冲区匹配 `GetFriendshipRequiredPointByRank(int32, int32&)` 的 UFunction
 *          布局，避免手工对齐；查询失败时返回 `fallback`。
 */
[[nodiscard]] auto friendship_required_point(const int rank, const int fallback) -> int {
    auto* const db = database();
    auto* const function =
        db == nullptr ? nullptr
                      : db->GetFunctionByNameInChain(STR("GetFriendshipRequiredPointByRank"));
    if (function == nullptr) {
        return fallback;
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
        return fallback;
    }
    rankProp->SetPropertyValueInContainer(buffer.data(), static_cast<int32_t>(rank));
    db->ProcessEvent(function, buffer.data());
    return outProp->GetPropertyValueInContainer(buffer.data());
}

/** @brief 调用 `AddFriendShip(int32, bool)` 增加亲密度点数并应用伙伴技能效果。 */
auto add_friendship(UObject* pal, const int delta) -> void {
    auto* const function = pal->GetFunctionByNameInChain(STR("AddFriendShip"));
    if (function == nullptr) {
        return;
    }
    struct Params {
        int32_t Value{};
        bool bApplyPassiveSkill{};
    } params{.Value = static_cast<int32_t>(delta), .bApplyPassiveSkill = true};
    pal->ProcessEvent(function, &params);
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
    snapshot.level = invoke_int_return(pal, STR("GetLevel"));
    snapshot.friendshipRank = invoke_int_return(pal, STR("GetFriendshipRank"));
    snapshot.friendshipPoint = invoke_int_return(pal, STR("GetFriendshipPoint"));

    FStructProperty* saveProperty = nullptr;
    void* const saveParam = save_parameter_slot(pal, saveProperty);
    if (saveParam == nullptr) {
        return snapshot;
    }
    auto* const rowStruct = saveProperty->GetStruct().Get();
    snapshot.talentHp = read_byte(rowStruct, saveParam, STR("Talent_HP"));
    snapshot.talentShot = read_byte(rowStruct, saveParam, STR("Talent_Shot"));
    snapshot.talentDefense = read_byte(rowStruct, saveParam, STR("Talent_Defense"));
    snapshot.readable = true;
    return snapshot;
}

auto PalStatGateway::apply_stat_edit(const PalStatTarget target, const PalStatEditRequest& request)
    -> bool {
    auto* const pal = to_pal(target);
    if (pal == nullptr) {
        return false;
    }
    FStructProperty* saveProperty = nullptr;
    void* const saveParam = save_parameter_slot(pal, saveProperty);
    if (saveParam == nullptr) {
        return false;
    }
    auto* const rowStruct = saveProperty->GetStruct().Get();

    if (request.values.level.has_value()) {
        write_byte(rowStruct, saveParam, STR("Level"), *request.values.level, clamp_level);
    }
    if (request.values.talentHp.has_value()) {
        write_byte(rowStruct, saveParam, STR("Talent_HP"), *request.values.talentHp, clamp_talent);
    }
    if (request.values.talentShot.has_value()) {
        write_byte(rowStruct, saveParam, STR("Talent_Shot"), *request.values.talentShot,
                   clamp_talent);
    }
    if (request.values.talentDefense.has_value()) {
        write_byte(rowStruct, saveParam, STR("Talent_Defense"), *request.values.talentDefense,
                   clamp_talent);
    }
    if (request.values.friendshipRank.has_value() && pal_game::is_valid(pal)) {
        const int targetRank = clamp_friendship_rank(*request.values.friendshipRank);
        const int currentRank = invoke_int_return(pal, STR("GetFriendshipRank"));
        const int requiredPoint = friendship_required_point(targetRank, kFriendshipPointFallback);
        if (targetRank >= currentRank) {
            const int currentPoint = invoke_int_return(pal, STR("GetFriendshipPoint"));
            add_friendship(pal, std::max(0, requiredPoint - currentPoint));
        } else {
            auto* const pointProp = CastField<FIntProperty>(
                rowStruct->FindProperty(FName(STR("FriendshipPoint"), FNAME_Find)));
            if (pointProp != nullptr) {
                pointProp->SetPropertyValueInContainer(saveParam,
                                                       static_cast<int32_t>(requiredPoint));
            }
        }
    }
    return true;
}
}  // namespace pal_stats
