/**
 * @file grapple_cooldown_gateway.cpp
 * @brief 实现严格按物品 Raw ID 识别、可逆且不跨帧缓存 UObject 的爪钩冷却网关。
 */
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/text_encoding.hpp>
#include <game/pal_game.hpp>
#include <grappling_hook/cooldown_gateway.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace grappling_hook {
using pal_game::find_object_by_full_name;
namespace {
/** @brief 无冷却使用的小正数，避免部分游戏路径把零解释为尚未初始化。 */
inline constexpr float kNoCooldownSeconds = 0.1F;
/** @brief 浮点重读验证容差。 */
inline constexpr float kVerificationTolerance = 0.0001F;

/** @brief 当前调用内的一条已完整预检的写入计划；绝不跨调用保存。 */
struct CooldownWritePlan {
    UObject* object{};           /**< 非拥有、仅当前游戏线程调用有效的武器对象。 */
    FFloatProperty* property{};  /**< 非拥有、仅当前调用有效的冷却属性。 */
    std::wstring objectFullName; /**< 用于生成跨帧纯值恢复记录的完整对象名。 */
    float originalCooldown{};    /**< 本次应用前读取的对象原值。 */
};

/** @brief 从武器的 `ownItemID.StaticId` 严格读取 Raw ID；结构不匹配时返回空。 */
[[nodiscard]] auto weapon_item_id(UObject* weapon) -> std::string {
    if (!pal_game::is_valid(weapon)) {
        return {};
    }
    auto* const itemIdProperty =
        CastField<FStructProperty>(weapon->GetPropertyByNameInChain(STR("ownItemID")));
    if (itemIdProperty == nullptr) {
        return {};
    }
    auto* const itemIdStruct = itemIdProperty->GetStruct().Get();
    if (itemIdStruct == nullptr) {
        return {};
    }
    void* const itemId = itemIdProperty->ContainerPtrToValuePtr<void>(weapon);
    auto* const staticIdProperty = itemIdStruct->FindProperty(FName(STR("StaticId"), FNAME_Find));
    auto* const staticId = staticIdProperty == nullptr
                               ? nullptr
                               : staticIdProperty->ContainerPtrToValuePtr<FName>(itemId);
    return staticId == nullptr ? std::string{} : text_encoding::to_utf8(staticId->ToString());
}

/** @brief 判断两个冷却浮点值是否可视为相等。 */
[[nodiscard]] auto nearly_equal(const float left, const float right) noexcept -> bool {
    return std::abs(left - right) <= kVerificationTolerance;
}

/** @brief 恢复当前调用内已写入的计划，用于应用阶段的即时失败回滚。 */
auto restore_plans(const std::vector<CooldownWritePlan>& plans) -> bool {
    bool restored = true;
    for (const auto& plan : plans) {
        if (!pal_game::is_valid(plan.object) || plan.property == nullptr) {
            restored = false;
            continue;
        }
        plan.property->SetPropertyValueInContainer(plan.object, plan.originalCooldown);
        restored = nearly_equal(plan.property->GetPropertyValueInContainer(plan.object),
                                plan.originalCooldown) &&
                   restored;
    }
    return restored;
}
}  // namespace

auto GrappleCooldownGateway::apply() -> CooldownGatewayResult {
    std::vector<UObject*> weapons;
    UObjectGlobals::FindAllOf(STR("PalWeaponBase"), weapons);

    std::vector<CooldownWritePlan> plans;
    plans.reserve(weapons.size());
    for (auto* const weapon : weapons) {
        if (!pal_game::is_valid(weapon)) {
            continue;
        }
        const auto itemId = weapon_item_id(weapon);
        if (!is_grappling_item_id(itemId)) {
            continue;
        }

        auto* const property =
            CastField<FFloatProperty>(weapon->GetPropertyByNameInChain(STR("CoolDownTime")));
        if (property == nullptr) {
            return {
                .status = CooldownGatewayStatus::layoutUnavailable,
                .message = "未修改爪钩冷却：正式爪钩对象缺少 CoolDownTime 字段。",
            };
        }
        plans.push_back({
            .object = weapon,
            .property = property,
            .objectFullName = std::wstring{weapon->GetFullName()},
            .originalCooldown = property->GetPropertyValueInContainer(weapon),
        });
    }
    if (plans.empty()) {
        return {
            .status = CooldownGatewayStatus::targetUnavailable,
            .message = "未找到当前已生成且物品 ID 可确认的爪钩枪；重新装备后再开启。",
        };
    }

    for (auto& plan : plans) {
        if (!pal_game::is_valid(plan.object)) {
            const bool restored = restore_plans(plans);
            return {
                .status = CooldownGatewayStatus::verificationFailed,
                .message = restored ? "爪钩对象在应用期间失效，已恢复原值。"
                                    : "爪钩对象在应用期间失效，且原值恢复未能完整确认。",
            };
        }
        plan.property->SetPropertyValueInContainer(plan.object, kNoCooldownSeconds);
    }
    for (const auto& plan : plans) {
        if (!pal_game::is_valid(plan.object) ||
            !nearly_equal(plan.property->GetPropertyValueInContainer(plan.object),
                          kNoCooldownSeconds)) {
            const bool restored = restore_plans(plans);
            return {
                .status = CooldownGatewayStatus::verificationFailed,
                .message = restored ? "爪钩冷却写后验证失败，已恢复所有原值。"
                                    : "爪钩冷却写后验证和原值恢复均未能完整确认。",
            };
        }
    }

    CooldownGatewayResult result{
        .status = CooldownGatewayStatus::succeeded,
        .message = "已对当前正式爪钩枪应用无冷却，并保存各对象原值。",
    };
    result.records.reserve(plans.size());
    for (auto& plan : plans) {
        result.records.push_back({
            .objectFullName = std::move(plan.objectFullName),
            .originalCooldown = plan.originalCooldown,
        });
    }
    return result;
}

auto GrappleCooldownGateway::restore(const std::span<const CooldownOverrideRecord> records)
    -> CooldownGatewayResult {
    std::vector<CooldownWritePlan> plans;
    plans.reserve(records.size());
    for (const auto& record : records) {
        auto* const weapon = find_object_by_full_name(record.objectFullName);
        if (!pal_game::is_valid(weapon)) {
            continue;
        }
        if (!is_grappling_item_id(weapon_item_id(weapon))) {
            return {
                .status = CooldownGatewayStatus::layoutUnavailable,
                .message = "未恢复爪钩冷却：账本对象路径已不再指向正式爪钩枪。",
            };
        }
        auto* const property =
            CastField<FFloatProperty>(weapon->GetPropertyByNameInChain(STR("CoolDownTime")));
        if (property == nullptr) {
            return {
                .status = CooldownGatewayStatus::layoutUnavailable,
                .message = "未恢复爪钩冷却：当前对象缺少 CoolDownTime 字段。",
            };
        }
        const float current = property->GetPropertyValueInContainer(weapon);
        if (!nearly_equal(current, kNoCooldownSeconds)) {
            continue;
        }
        plans.push_back({
            .object = weapon,
            .property = property,
            .objectFullName = record.objectFullName,
            .originalCooldown = record.originalCooldown,
        });
    }

    if (!restore_plans(plans)) {
        return {
            .status = CooldownGatewayStatus::verificationFailed,
            .message = "爪钩冷却原值恢复未能完整验证；已保留账本供下一次恢复。",
        };
    }
    return {
        .status = CooldownGatewayStatus::succeeded,
        .message = "爪钩冷却已按每个对象的覆盖前原值恢复。",
    };
}

}  // namespace grappling_hook
