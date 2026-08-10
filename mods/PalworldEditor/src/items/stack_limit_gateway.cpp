/**
 * @file stack_limit_gateway.cpp
 * @brief 实现仅修改原生可堆叠物品、写后验证且可恢复的堆叠上限事务。
 */
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/text_encoding.hpp>
#include <game/pal_game.hpp>
#include <items/stack_limit_gateway.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace item_stack_limit {
namespace {
inline constexpr const TCHAR* kStaticItemDataClassPath = STR("/Script/Pal.PalStaticItemDataBase");

/** @brief 当前同步事务内的一条完整预检写计划；绝不跨调用保存。 */
struct StackLimitWritePlan {
    UObject* object{};
    FIntProperty* property{};
    std::wstring objectFullName;
    std::string itemId;
    std::int32_t originalLimit{};
};

/** @brief 解析物品 Raw ID；缺少精确 FName 属性时返回空。 */
[[nodiscard]] auto item_id(UObject* object) -> std::string {
    if (!pal_game::is_valid(object)) {
        return {};
    }
    auto* const property = CastField<FNameProperty>(object->GetPropertyByNameInChain(STR("ID")));
    if (property == nullptr) {
        return {};
    }
    return text_encoding::to_utf8(property->GetPropertyValueInContainer(object).ToString());
}

/** @brief 把当前调用内已写入计划恢复到各自原值，并返回仍无法验证的责任记录。 */
[[nodiscard]] auto rollback_plans(const std::span<const StackLimitWritePlan> plans)
    -> std::vector<StackLimitOverrideRecord> {
    std::vector<StackLimitOverrideRecord> remaining;
    remaining.reserve(plans.size());
    for (const auto& plan : plans) {
        bool restored{};
        if (pal_game::is_valid(plan.object) && plan.property != nullptr) {
            plan.property->SetPropertyValueInContainer(plan.object, plan.originalLimit);
            restored =
                plan.property->GetPropertyValueInContainer(plan.object) == plan.originalLimit;
        }
        if (!restored) {
            remaining.push_back({
                .objectFullName = plan.objectFullName,
                .itemId = plan.itemId,
                .originalLimit = plan.originalLimit,
            });
        }
    }
    return remaining;
}
}  // namespace

auto apply_stack_limit_override() -> StackLimitGatewayResult {
    auto* const staticItemDataClass =
        UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kStaticItemDataClassPath);
    if (!pal_game::is_valid(staticItemDataClass)) {
        return {
            .status = StackLimitGatewayStatus::preflightFailed,
            .message = "未修改堆叠上限：缺少 PalStaticItemDataBase 运行时类型。",
        };
    }

    std::vector<UObject*> objects;
    UObjectGlobals::FindAllOf(STR("PalStaticItemDataBase"), objects);

    std::vector<StackLimitWritePlan> plans;
    plans.reserve(objects.size());
    std::unordered_set<std::wstring> objectNames;
    objectNames.reserve(objects.size());
    for (auto* const object : objects) {
        if (!pal_game::is_valid(object) || !object->IsA(staticItemDataClass)) {
            continue;
        }
        auto* const property =
            CastField<FIntProperty>(object->GetPropertyByNameInChain(STR("MaxStackCount")));
        if (property == nullptr) {
            return {
                .status = StackLimitGatewayStatus::preflightFailed,
                .message = "未修改堆叠上限：物品静态数据缺少精确 int32 MaxStackCount 字段。",
            };
        }

        const auto originalLimit = property->GetPropertyValueInContainer(object);
        if (!is_expandable_limit(originalLimit)) {
            continue;
        }
        const auto id = item_id(object);
        if (id.empty()) {
            return {
                .status = StackLimitGatewayStatus::preflightFailed,
                .message = "未修改堆叠上限：候选物品缺少精确且非空的 FName ID 字段。",
            };
        }
        std::wstring objectFullName{object->GetFullName()};
        if (objectFullName.empty() || !objectNames.emplace(objectFullName).second) {
            return {
                .status = StackLimitGatewayStatus::preflightFailed,
                .message = "未修改堆叠上限：物品静态数据对象身份为空或重复。",
            };
        }
        plans.push_back({
            .object = object,
            .property = property,
            .objectFullName = std::move(objectFullName),
            .itemId = id,
            .originalLimit = originalLimit,
        });
    }

    if (plans.empty()) {
        return {
            .status = StackLimitGatewayStatus::targetUnavailable,
            .message = "未找到已加载且原始上限为 9999 的普通可堆叠物品；可关闭后重新开启重试。",
        };
    }

    std::vector<StackLimitWritePlan> written;
    written.reserve(plans.size());
    for (const auto& plan : plans) {
        if (!pal_game::is_valid(plan.object)) {
            auto remaining = rollback_plans(written);
            return {
                .status = remaining.empty() ? StackLimitGatewayStatus::verificationFailedRolledBack
                                            : StackLimitGatewayStatus::rollbackFailed,
                .records = std::move(remaining),
                .message = "物品对象在应用期间失效；已尝试恢复此前写入。",
            };
        }

        written.push_back(plan);
        // PalStaticItemDataBase 没有项目内已验证的原生 setter/OnRep；该静态字段以 Property API
        // 直接写入，并以同步重读作为当前运行时的可观察验证。
        plan.property->SetPropertyValueInContainer(plan.object, kExpandedStackLimit);
        if (plan.property->GetPropertyValueInContainer(plan.object) != kExpandedStackLimit) {
            auto remaining = rollback_plans(written);
            return {
                .status = remaining.empty() ? StackLimitGatewayStatus::verificationFailedRolledBack
                                            : StackLimitGatewayStatus::rollbackFailed,
                .records = std::move(remaining),
                .message = "堆叠上限写后验证失败；已尝试恢复本事务实际写入。",
            };
        }
    }

    StackLimitGatewayResult result{
        .status = StackLimitGatewayStatus::succeeded,
        .message = "已将普通可堆叠物品上限设置为 999,999,999；特殊上限物品保持不变。",
    };
    result.records.reserve(plans.size());
    for (auto& plan : plans) {
        result.records.push_back({
            .objectFullName = std::move(plan.objectFullName),
            .itemId = std::move(plan.itemId),
            .originalLimit = plan.originalLimit,
        });
    }
    return result;
}

auto restore_stack_limit_override(const std::span<const StackLimitOverrideRecord> records)
    -> StackLimitGatewayResult {
    auto* const staticItemDataClass =
        UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kStaticItemDataClassPath);
    if (!pal_game::is_valid(staticItemDataClass)) {
        std::vector<StackLimitOverrideRecord> remaining{records.begin(), records.end()};
        return {
            .status = StackLimitGatewayStatus::restorationFailed,
            .records = std::move(remaining),
            .message = "未能恢复堆叠上限：缺少 PalStaticItemDataBase 运行时类型。",
        };
    }

    std::vector<StackLimitOverrideRecord> remaining;
    remaining.reserve(records.size());
    for (const auto& record : records) {
        auto* const object = pal_game::find_object_by_full_name(record.objectFullName);
        if (!pal_game::is_valid(object)) {
            remaining.push_back(record);
            continue;
        }
        if (!object->IsA(staticItemDataClass) || item_id(object) != record.itemId) {
            remaining.push_back(record);
            continue;
        }
        auto* const property =
            CastField<FIntProperty>(object->GetPropertyByNameInChain(STR("MaxStackCount")));
        if (property == nullptr) {
            remaining.push_back(record);
            continue;
        }

        const auto current = property->GetPropertyValueInContainer(object);
        if (current == record.originalLimit) {
            continue;
        }
        if (current != kExpandedStackLimit) {
            // 其他模块已在本 mod 之后改写；不得用旧快照覆盖其当前值。
            continue;
        }
        property->SetPropertyValueInContainer(object, record.originalLimit);
        if (property->GetPropertyValueInContainer(object) != record.originalLimit) {
            remaining.push_back(record);
        }
    }

    if (!remaining.empty()) {
        const auto unresolvedCount = remaining.size();
        return {
            .status = StackLimitGatewayStatus::restorationFailed,
            .records = std::move(remaining),
            .message =
                "未能完整恢复堆叠上限；仍保留 " + std::to_string(unresolvedCount) + " 条恢复责任。",
        };
    }

    return {
        .status = StackLimitGatewayStatus::succeeded,
        .message = "物品堆叠上限已按每个对象的覆盖前原值恢复。",
    };
}

}  // namespace item_stack_limit
