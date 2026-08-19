/**
 * @file revive_timer_gateway.cpp
 * @brief 实现终端复活计时的解析、写入验证与可逆恢复。
 * @details 复用 pal_stats 的 PalUtility:GetGameSetting 反射路径；所有指针仅在
 *          当前游戏线程调用内使用，跨帧只保存原值 float。
 */
#include <utility>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/game_reflection.hpp>
#include <revive_timer/revive_timer_gateway.hpp>

namespace revive_timer {
using namespace RC;
using namespace RC::Unreal;

namespace {

/** @brief 解析结果分类：暂不可用（可重试）与结构不兼容（fail-closed）分开。 */
enum class ResolveStatus : std::uint8_t {
    unavailable,
    incompatible,
    ready,
};

/** @brief 已校验的复活计时字段句柄；仅在当前调用内有效。 */
struct ReviveTimerAccess {
    ResolveStatus status{ResolveStatus::unavailable};
    UObject* setting{};
    FFloatProperty* property{};
};

/**
 * @brief 解析当前世界的 PalGameSetting 与 PalBoxReviveTime 字段。
 * @retval unavailable 世界上下文或设置实例暂不可用（可重试）。
 * @retval incompatible 函数/字段签名漂移（SDK 不兼容）。
 */
[[nodiscard]] auto resolve_access() -> ReviveTimerAccess {
    auto* const utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* const function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalUtility:GetGameSetting"));
    if (utility == nullptr || function == nullptr) {
        return {.status = ResolveStatus::incompatible};
    }
    auto* const contextProperty = CastField<FObjectPropertyBase>(
        function->FindProperty(FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const resultProperty = CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 2) ||
        !pal_game::is_input_parameter(contextProperty) ||
        !pal_game::is_return_parameter(resultProperty)) {
        return {.status = ResolveStatus::incompatible};
    }

    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    if (!pal_game::is_valid(worldContext)) {
        return {};
    }
    pal_game::FunctionParams params{function};
    contextProperty->SetObjectPropertyValue(
        contextProperty->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    utility->ProcessEvent(function, params.data());
    auto* const setting = resultProperty->GetObjectPropertyValue(
        resultProperty->ContainerPtrToValuePtr<void>(params.data()));
    if (!pal_game::is_valid(setting)) {
        return {};
    }

    auto* const property =
        CastField<FFloatProperty>(setting->GetPropertyByNameInChain(STR("PalBoxReviveTime")));
    if (property == nullptr) {
        return {.status = ResolveStatus::incompatible, .setting = setting};
    }
    return {.status = ResolveStatus::ready, .setting = setting, .property = property};
}

}  // namespace

auto apply_revive_timer_override() -> ReviveTimerApplyResult {
    const auto access = resolve_access();
    if (access.status == ResolveStatus::unavailable) {
        return {.status = ReviveTimerGatewayStatus::targetUnavailable,
                .original = 0.0F,
                .message = "世界或游戏设置暂未就绪；稍后可重新检测。"};
    }
    if (access.status == ResolveStatus::incompatible || access.property == nullptr) {
        return {.status = ReviveTimerGatewayStatus::preflightFailed,
                .original = 0.0F,
                .message = "未修改复活计时：GetGameSetting 签名或 PalBoxReviveTime 字段不兼容。"};
    }

    const float original = access.property->GetPropertyValueInContainer(access.setting);
    if (original == kRemovedReviveSeconds) {
        return {.status = ReviveTimerGatewayStatus::succeeded,
                .original = original,
                .message = "复活计时原本即为 0；已记录原值，无需写入。"};
    }
    access.property->SetPropertyValueInContainer(access.setting, kRemovedReviveSeconds);
    if (access.property->GetPropertyValueInContainer(access.setting) == kRemovedReviveSeconds) {
        return {.status = ReviveTimerGatewayStatus::succeeded,
                .original = original,
                .message = "已移除终端复活计时。"};
    }

    access.property->SetPropertyValueInContainer(access.setting, original);
    if (access.property->GetPropertyValueInContainer(access.setting) == original) {
        return {.status = ReviveTimerGatewayStatus::verifiedRollback,
                .original = original,
                .message = "复活计时写入验证失败；原值已恢复。"};
    }
    return {.status = ReviveTimerGatewayStatus::rollbackFailed,
            .original = original,
            .message = "复活计时写入与回滚验证均失败；保留恢复责任。"};
}

auto restore_revive_timer_override(const float original) -> ReviveTimerRestoreResult {
    const auto access = resolve_access();
    if (access.status == ResolveStatus::unavailable) {
        // 设置实例已随世界销毁或暂不可用；复活计时由新世界实例的原生值承载，无需恢复。
        return {.status = ReviveTimerGatewayStatus::succeeded,
                .message = "游戏设置实例不可用；无需恢复复活计时。"};
    }
    if (access.status == ResolveStatus::incompatible || access.property == nullptr) {
        return {.status = ReviveTimerGatewayStatus::rollbackFailed,
                .message = "恢复复活计时失败：字段或函数签名不兼容。"};
    }

    const float current = access.property->GetPropertyValueInContainer(access.setting);
    if (current != kRemovedReviveSeconds) {
        // 当前值已不是本 mod 写入的 0（对象被重建或值被游戏改写）；责任已不存在。
        return {.status = ReviveTimerGatewayStatus::succeeded,
                .message = "复活计时已不是本 mod 的覆盖值；无需恢复。"};
    }
    access.property->SetPropertyValueInContainer(access.setting, original);
    if (access.property->GetPropertyValueInContainer(access.setting) == original) {
        return {.status = ReviveTimerGatewayStatus::succeeded,
                .message = "已恢复终端复活计时原值。"};
    }
    return {.status = ReviveTimerGatewayStatus::rollbackFailed,
            .message = "恢复复活计时后重读验证失败；保留恢复责任。"};
}

}  // namespace revive_timer
