/**
 * @file pal_base_camp_reflection.hpp
 * @brief 远程终端与资源共享共用的单次基地反射调用原语。
 * @details 仅统一签名验证与一次调用，不包含两项功能各自的权限、容错或重试策略。
 */
#pragma once

#include <vector>

#include <Unreal/UnrealCoreStructs.hpp>

namespace RC::Unreal {
class UObject;
}

namespace pal_base_camp_reflection {
/** @brief 调用 PalBaseCampManager:GetBaseCampIds。 */
[[nodiscard]] auto read_base_ids(RC::Unreal::UObject* manager,
                                 std::vector<RC::Unreal::FGuid>& output) -> bool;

/** @brief 调用 PalBaseCampManager:TryGetModel。 */
[[nodiscard]] auto try_get_base_model(RC::Unreal::UObject* manager, const RC::Unreal::FGuid& baseId,
                                      RC::Unreal::UObject*& model) -> bool;

/** @brief 调用 PalMapObjectManager:FindConcreteModel。 */
[[nodiscard]] auto find_concrete_model(RC::Unreal::UObject* manager,
                                       const RC::Unreal::FGuid& instanceId,
                                       RC::Unreal::UObject*& concreteModel) -> bool;

/**
 * @brief 解析本地玩家所属公会的 GUID。
 * @details 链路：PalUtility:GetLocalPalPlayerController → 控制器 GetPlayerUId →
 *          PalUtility:GetGuildByPlayerUId → 公会 GetId。任一环节签名漂移或返回无效
 *          对象即失败（fail-closed），由调用方决定拦截语义。
 */
[[nodiscard]] auto resolve_local_guild_id(RC::Unreal::UObject* worldContext,
                                          RC::Unreal::FGuid& guildId) -> bool;
}  // namespace pal_base_camp_reflection
