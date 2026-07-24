/**
 * @file pal_skills.hpp
 * @brief 声明把技能编辑领域服务适配到 Palworld Unreal 反射接口的网关。
 */
#pragma once

#include <unordered_map>

#include <skills/skill_catalog.hpp>
#include <skills/skill_editor_service.hpp>

/** @brief 提供 Palworld 特定的技能读取、写入和运行时目录加载能力。 */
namespace pal_skills {
/**
 * @brief 通过 `PalIndividualCharacterParameter` 反射 API 实现技能编辑网关。
 * @details 本类不拥有任何 Unreal 对象。所有成员函数都必须在 Unreal 初始化完成后的游戏线程调用；
 *          写接口的布尔返回值只表示能否发起反射调用，调用方仍需重读状态确认游戏是否接受修改。
 */
class PalSkillGateway final : public skill_editor::ISkillGateway {
public:
    /**
     * @brief 检查目标句柄是否仍指向可访问的帕鲁 UObject。
     * @param[in] target 由非拥有 UObject 指针编码的技能目标句柄。
     * @retval true 目标当前通过轻量 UObject 有效性检查。
     * @retval false 目标为空或已经失效。
     */
    [[nodiscard]] auto is_valid(skill_editor::SkillTarget target) const -> bool override;

    /**
     * @brief 读取帕鲁当前的被动技能与前三个 `EquipWaza` 主动技能槽。
     * @param[in] target 已由 is_valid() 校验的技能目标句柄。
     * @return 从游戏反射接口读取的实际技能状态；目标失效时返回空状态。
     */
    auto read_state(skill_editor::SkillTarget target) -> skill_editor::SkillState override;

    /**
     * @brief 请求向帕鲁添加一个被动技能。
     * @param[in] target 已由 is_valid() 校验的技能目标句柄。
     * @param[in] id 要传给 `AddPassiveSkill` 的 ASCII Raw ID。
     * @retval true 目标、ID 和反射函数有效，且已经发起调用。
     * @retval false 无法安全发起反射调用。
     * @note 返回 `true` 不代表游戏一定接受该技能，调用方必须重读验证。
     */
    auto add_passive(skill_editor::SkillTarget target, std::string_view id) -> bool override;

    /**
     * @brief 请求从帕鲁移除一个被动技能。
     * @param[in] target 已由 is_valid() 校验的技能目标句柄。
     * @param[in] id 要传给 `RemovePassiveSkill` 的 ASCII Raw ID。
     * @retval true 目标、ID 和反射函数有效，且已经发起调用。
     * @retval false 无法安全发起反射调用。
     * @note 返回 `true` 不代表游戏一定接受修改，调用方必须重读验证。
     */
    auto remove_passive(skill_editor::SkillTarget target, std::string_view id) -> bool override;

    /**
     * @brief 按给定顺序重写帕鲁的全部 `EquipWaza` 主动技能槽。
     * @param[in] target 已由 is_valid() 校验的技能目标句柄。
     * @param[in] skills 期望的紧凑槽位序列，顺序即槽位顺序，最多包含 3 项。
     * @retval true 已先清空现有槽位并按输入顺序发起全部添加调用。
     * @retval false 目标、反射函数或输入数量无效，或写入期间目标失效。
     * @warning 失败可能发生在清空之后，调用方必须通过重读和回滚恢复原始序列。
     */
    auto rewrite_active(skill_editor::SkillTarget target,
                        std::span<const skill_editor::ActiveSkill> skills) -> bool override;

    /**
     * @brief 从当前运行时加载全部可分配被动技能和主动技能枚举值。
     * @return 技能目录快照；任一目录为空时 `ready` 为 `false` 并设置 `error`。
     * @details 本方法自行获取稳定的玩家背包世界上下文。成功时会同步重建主动技能数值到
     *          Raw ID 的内部映射，供 read_state() 使用。
     */
    auto load_catalog() -> skill_editor::SkillCatalogSnapshot;

private:
    /**
     * @brief 最近一次成功目录加载生成的 `EPalWazaID` 数值到 Raw ID 映射。
     * @details 此值类型缓存不拥有 Unreal 对象，仅在游戏线程由 load_catalog() 重建并由
     *          read_state() 读取。
     */
    std::unordered_map<std::uint16_t, std::string> activeIds_;
};
}  // namespace pal_skills
