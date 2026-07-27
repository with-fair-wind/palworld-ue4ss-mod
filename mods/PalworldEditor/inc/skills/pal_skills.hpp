/**
 * @file pal_skills.hpp
 * @brief 声明把技能编辑领域服务适配到 Palworld Unreal 反射接口的网关。
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <span>

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
     * @details 主动技能数值通过 Palworld 1.0 生成定义表还原为 Raw ID，未知值回退为十进制文本。
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
     * @brief 在数量和时间软预算内读取一批被动技能分类元数据。
     * @param[in] ids 按目录顺序排列的待读取 Raw ID。
     * @param[in] maxItems 本批最多实际调用 `GetSkillData` 的 ID 数。
     * @param[in] budget 本批游戏线程软时间预算；每次 `ProcessEvent` 后检查。
     * @return 已完成条目、结构性错误和实际耗时组成的纯值结果。
     * @details manager、函数、属性和参数缓冲区均只在本次调用内有效，不跨 EngineTick 缓存。
     *          单个 ID 未找到以空 metadata 返回，不会中止批次。
     */
    [[nodiscard]] auto load_passive_skill_metadata_batch(std::span<const std::string> ids,
                                                         std::size_t maxItems,
                                                         std::chrono::microseconds budget) const
        -> skill_editor::PassiveSkillMetadataBatchResult;

    /**
     * @brief 加载全部可分配被动技能和生成的 Palworld 1.0 主动技能定义。
     * @return 被动与主动区段分别报告可用状态和最近错误的技能目录快照。
     * @details `PalPlayerInventoryData` 仅作为当前语言名称查询的世界上下文；上下文暂不可用时
     *          目录仍以 Raw ID 可用。一类目录失败不会清空另一类目录。
     */
    auto load_catalog() -> skill_editor::SkillCatalogSnapshot;
};
}  // namespace pal_skills
