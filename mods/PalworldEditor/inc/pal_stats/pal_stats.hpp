/**
 * @file pal_stats.hpp
 * @brief 声明把帕鲁属性编辑领域服务适配到 Palworld Unreal 反射接口的网关。
 */
#pragma once

#include <pal_stats/pal_stat_editor.hpp>

/** @brief 提供 Palworld 特定的帕鲁属性读取与写入能力。 */
namespace pal_stats {
/**
 * @brief 通过 `PalIndividualCharacterParameter.SaveParameter` 反射读写帕鲁属性。
 * @details 本类不拥有任何 Unreal 对象。所有成员函数都必须在游戏线程调用；
 *          `apply_stat_edit` 的布尔返回值只表示能否发起写入，调用方仍需重读确认。
 */
class PalStatGateway final {
public:
    /**
     * @brief 检查目标句柄是否仍指向可访问的帕鲁 UObject。
     * @param[in] target 由非拥有 UObject 指针编码的目标句柄。
     * @retval true 目标当前通过轻量 UObject 有效性检查。
     * @retval false 目标为空或已失效。
     */
    [[nodiscard]] auto is_valid(PalStatTarget target) const -> bool;

    /**
     * @brief 读取当前等级、三项个体值与亲密度 rank/point。
     * @param[in] target 已由 is_valid() 校验的目标句柄。
     * @return 从游戏反射读取的属性快照；目标失效或结构不可用时 `readable == false`。
     */
    [[nodiscard]] auto read_stats(PalStatTarget target) -> PalStatSnapshot;

    /**
     * @brief 按 `request.values` 写入各项属性；空 optional 跳过该项。
     * @param[in] target 已由 is_valid() 校验的目标句柄。
     * @param[in] request 携带期望值与目标/世界代次的编辑请求。
     * @retval true 目标与 `SaveParameter` 结构有效，已对每个设置项发起写入。
     * @retval false 目标失效或 `SaveParameter` 结构不可达。
     * @note 返回 `true` 不保证游戏立即刷新面板；按设计在重召唤/重载后可见。
     */
    auto apply_stat_edit(PalStatTarget target, const PalStatEditRequest& request) -> bool;
};
}  // namespace pal_stats
