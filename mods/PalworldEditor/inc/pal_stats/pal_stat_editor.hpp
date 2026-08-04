/**
 * @file pal_stat_editor.hpp
 * @brief 与 Unreal 解耦的帕鲁属性编辑领域模型、差量草稿与线程安全请求槽。
 * @details 本文件只依赖标准库；具体游戏读写由 `PalStatGateway` 实现。
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

/** @brief 提供帕鲁等级/个体值/亲密度编辑的纯值领域模型。 */
namespace pal_stats {
/** @brief 由非拥有 `UObject*` 编码的临时帕鲁目标句柄；使用前必须由网关校验。 */
using PalStatTarget = std::uintptr_t;

/** @brief 个体存档中的帕鲁性别；普通帕鲁只允许提交 male/female。 */
enum class PalGender : std::uint8_t {
    none,
    male,
    female,
};

/** @brief Palworld 1.0.2 可编辑的具体工作适应性值，与游戏枚举数值保持一致。 */
enum class WorkSuitability : std::uint8_t {
    emitFlame = 1,
    watering,
    seeding,
    generateElectricity,
    handcraft,
    collection,
    deforest,
    mining,
    oilExtraction,
    productMedicine,
    cool,
    transport,
    monsterFarm,
};

inline constexpr std::size_t kWorkSuitabilityCount = 13;
/** @brief 工作适应性附加数组允许的最大条目数；只用于运行时安全校验。 */
inline constexpr int kMaxWorkSuitabilityEntries = 13;
using WorkSuitabilityRanks = std::array<int, kWorkSuitabilityCount>;

/** @return 具体工作适应性在固定等级数组中的零基索引。 */
[[nodiscard]] constexpr auto work_suitability_index(const WorkSuitability value) noexcept
    -> std::size_t {
    return static_cast<std::size_t>(value) - 1;
}

/** @brief 等级下限（不超过游戏满级，避免经验表空段）。 */
inline constexpr int kLevelMin = 1;
/** @brief 等级上限。 */
inline constexpr int kLevelMax = 80;
/** @brief 个体值下限。 */
inline constexpr int kTalentMin = 0;
/** @brief 普通个体值上限；拒绝生成超出游戏正常范围的存档数据。 */
inline constexpr int kTalentMax = 100;
/** @brief 帕鲁之魂强化等级下限。 */
inline constexpr int kSoulRankMin = 0;
/** @brief Palworld 1.0.2 四向帕鲁之魂强化等级上限。 */
inline constexpr int kSoulRankMax = 20;
/** @brief 浓缩机星级下限；内部 Rank 比 UI 星级大一。 */
inline constexpr int kCondensationStarsMin = 0;
/** @brief 亲密度 rank 下限。 */
inline constexpr int kFriendshipRankMin = 0;
/** @brief 亲密度 rank 上限。 */
inline constexpr int kFriendshipRankMax = 10;

/**
 * @brief 期望写入的属性值；空 `optional` 表示不改该项。
 */
struct PalStatValues {
    std::optional<int> level;             /**< 等级，clamp 到 `[1, 80]`。 */
    std::optional<int> talentHp;          /**< 个体值·HP（`Talent_HP`）。 */
    std::optional<int> talentShot;        /**< 个体值·攻击（`Talent_Shot`，远程）。 */
    std::optional<int> talentDefense;     /**< 个体值·防御（`Talent_Defense`）。 */
    std::optional<int> soulHpRank;        /**< 帕鲁之魂·最大 HP（`Rank_HP`）。 */
    std::optional<int> soulAttackRank;    /**< 帕鲁之魂·攻击（`Rank_Attack`）。 */
    std::optional<int> soulDefenseRank;   /**< 帕鲁之魂·防御（`Rank_Defence`）。 */
    std::optional<int> soulWorkSpeedRank; /**< 帕鲁之魂·工作速度（`Rank_CraftSpeed`）。 */
    std::optional<int> condensationStars; /**< 浓缩机 UI 星级；写入时同步内部 Rank/RankUpExp。 */
    std::optional<PalGender> gender;      /**< 雄性或雌性；none 会在预检阶段拒绝。 */
    std::optional<WorkSuitabilityRanks>
        workSuitabilityBonusRanks;     /**< 原生 setter 维护的永久附加工作适应性等级。 */
    std::optional<int> friendshipRank; /**< 亲密度 rank，clamp 到 `[0, 10]`。 */
};

/** @brief 由 UI 提交、等待游戏线程执行的一次属性编辑请求。 */
struct PalStatEditRequest {
    PalStatValues values;             /**< 期望写入的属性值。 */
    std::uint64_t targetGeneration{}; /**< GUI 提交时观察到的已确认目标代数。 */
    std::uint64_t worldGeneration{};  /**< GUI 提交时观察到的世界代次。 */
};

/** @brief 从游戏读取到的当前属性值，供 GUI 显示。 */
struct PalStatSnapshot {
    int level{};                       /**< 当前等级。 */
    int talentHp{};                    /**< 当前个体值·HP。 */
    int talentShot{};                  /**< 当前个体值·攻击。 */
    int talentDefense{};               /**< 当前个体值·防御。 */
    int soulHpRank{};                  /**< 当前最大 HP 强化等级。 */
    int soulAttackRank{};              /**< 当前攻击强化等级。 */
    int soulDefenseRank{};             /**< 当前防御强化等级。 */
    int soulWorkSpeedRank{};           /**< 当前工作速度强化等级。 */
    int condensationStars{};           /**< 当前 UI 浓缩星级。 */
    int condensationMaxStars{};        /**< 当前运行时允许的最大 UI 星级。 */
    int partnerSkillLevel{};           /**< 由内部 Rank 派生的伙伴技能等级，只读。 */
    int rankUpExp{};                   /**< 当前未完成的浓缩经验；直接设置星级时归零。 */
    PalGender gender{PalGender::none}; /**< 当前性别。 */
    int workSuitabilityMaxRank{};      /**< 当前运行时工作适应性等级上限。 */
    WorkSuitabilityRanks workSuitabilityBaseRanks{};  /**< 原生 getter 返回的基础等级。 */
    WorkSuitabilityRanks workSuitabilityBonusRanks{}; /**< 个体永久附加等级。 */
    WorkSuitabilityRanks workSuitabilityTotalRanks{}; /**< 基础等级与永久附加值之和，只读。 */
    int friendshipRank{};                             /**< 当前亲密度 rank。 */
    int friendshipPoint{};                            /**< 当前亲密度原始点数。 */
    bool readable{};                                  /**< 是否已成功读取过一次（目标选中后）。 */
};

/** @brief 一次属性事务的最终状态。 */
enum class PalStatEditStatus {
    succeeded,          /**< 请求字段写入后重读一致。 */
    rejected,           /**< 世界、目标或请求不再有效。 */
    preflightFailed,    /**< 反射布局或亲密度阈值在写入前不可用。 */
    verificationFailed, /**< 写入后重读值与请求不一致，但已成功恢复。 */
    rollbackFailed,     /**< 写入验证失败且恢复也未能确认。 */
};

/** @brief 提交给 GUI 的属性事务结果。 */
struct PalStatEditResult {
    PalStatEditStatus status{PalStatEditStatus::rejected}; /**< 事务终止状态。 */
    PalStatSnapshot snapshot;                              /**< 事务结束后的重读快照。 */
    std::string message;                                   /**< 面向用户的结果说明。 */
};

/** @brief 把等级限制到 `[kLevelMin, kLevelMax]`。 */
[[nodiscard]] inline auto clamp_level(const int value) -> int {
    return std::clamp(value, kLevelMin, kLevelMax);
}
/** @brief 把个体值限制到 `[kTalentMin, kTalentMax]`。 */
[[nodiscard]] inline auto clamp_talent(const int value) -> int {
    return std::clamp(value, kTalentMin, kTalentMax);
}
/** @brief 把帕鲁之魂强化等级限制到 `[kSoulRankMin, kSoulRankMax]`。 */
[[nodiscard]] inline auto clamp_soul_rank(const int value) -> int {
    return std::clamp(value, kSoulRankMin, kSoulRankMax);
}
/** @brief 按当前运行时上限限制浓缩机 UI 星级。 */
[[nodiscard]] inline auto clamp_condensation_stars(const int value, const int maxStars) -> int {
    return std::clamp(value, kCondensationStarsMin, std::max(kCondensationStarsMin, maxStars));
}
/** @brief 把 UI 星级转换为内部 `Rank`（0 星对应 Rank 1）。 */
[[nodiscard]] inline auto condensation_stars_to_internal_rank(const int stars, const int maxStars)
    -> int {
    return clamp_condensation_stars(stars, maxStars) + 1;
}
/** @brief 把内部 `Rank` 转换为 UI 星级。 */
[[nodiscard]] inline auto internal_rank_to_condensation_stars(const int rank, const int maxStars)
    -> int {
    return clamp_condensation_stars(rank - 1, maxStars);
}
/** @brief 永久附加工作适应性等级不得超过当前游戏运行时上限。 */
[[nodiscard]] inline auto clamp_work_suitability_bonus(const int value, const int maxRank) -> int {
    return std::clamp(value, 0, std::max(0, maxRank));
}
/** @brief 由原生基础等级和存档永久附加值计算面板所显示的合计等级。 */
[[nodiscard]] inline auto work_suitability_total_rank(const int baseRank, const int bonusRank,
                                                      const int maxRank) -> int {
    const int safeMax = std::max(0, maxRank);
    const int safeBase = std::clamp(baseRank, 0, safeMax);
    const int safeBonus = std::clamp(bonusRank, 0, safeMax - safeBase);
    return safeBase + safeBonus;
}
/**
 * @brief 计算一个方向允许编辑到的最大永久附加值。
 * @details 原生基础等级大于零时允许强化到运行时总上限。基础等级为零但已有附加值时，只允许
 *          保持或降低这个遗留值；从零开始的物种不允许凭空创建新的工作适应性。
 */
[[nodiscard]] inline auto max_editable_work_suitability_bonus(const int baseRank,
                                                              const int currentBonusRank,
                                                              const int maxRank) -> int {
    const int safeMax = std::max(0, maxRank);
    const int safeBase = std::clamp(baseRank, 0, safeMax);
    if (safeBase > 0) {
        return safeMax - safeBase;
    }
    return clamp_work_suitability_bonus(currentBonusRank, safeMax);
}
/** @brief 把期望的永久附加值转换为 `SetWorkSuitabilityAddRank` 所需的有符号增量。 */
[[nodiscard]] inline auto work_suitability_bonus_delta(const int current, const int desired,
                                                       const int maxRank) -> int {
    return clamp_work_suitability_bonus(desired, maxRank) - current;
}
/** @return 性别是否允许写入普通帕鲁存档。 */
[[nodiscard]] constexpr auto is_editable_gender(const PalGender gender) noexcept -> bool {
    return gender == PalGender::male || gender == PalGender::female;
}
/** @brief 把亲密度 rank 限制到 `[kFriendshipRankMin, kFriendshipRankMax]`。 */
[[nodiscard]] inline auto clamp_friendship_rank(const int value) -> int {
    return std::clamp(value, kFriendshipRankMin, kFriendshipRankMax);
}
/** @return `values` 是否至少设置了一个待写字段。 */
[[nodiscard]] inline auto has_any_change(const PalStatValues& values) -> bool {
    return values.level.has_value() || values.talentHp.has_value() ||
           values.talentShot.has_value() || values.talentDefense.has_value() ||
           values.soulHpRank.has_value() || values.soulAttackRank.has_value() ||
           values.soulDefenseRank.has_value() || values.soulWorkSpeedRank.has_value() ||
           values.condensationStars.has_value() || values.gender.has_value() ||
           values.workSuitabilityBonusRanks.has_value() || values.friendshipRank.has_value();
}

/** @return 请求是否包含工作适应性修改。 */
[[nodiscard]] inline auto has_work_suitability_change(const PalStatValues& values) -> bool {
    return values.workSuitabilityBonusRanks.has_value();
}

/** @return 请求是否包含工作适应性以外的属性修改。 */
[[nodiscard]] inline auto has_core_stat_change(const PalStatValues& values) -> bool {
    return values.level.has_value() || values.talentHp.has_value() ||
           values.talentShot.has_value() || values.talentDefense.has_value() ||
           values.soulHpRank.has_value() || values.soulAttackRank.has_value() ||
           values.soulDefenseRank.has_value() || values.soulWorkSpeedRank.has_value() ||
           values.condensationStars.has_value() || values.gender.has_value() ||
           values.friendshipRank.has_value();
}

/** @return 两组工作适应性等级在应用运行时上限后是否逐项一致。 */
[[nodiscard]] inline auto verify_work_suitability_ranks(const WorkSuitabilityRanks& expected,
                                                        const WorkSuitabilityRanks& actual,
                                                        const int maxRank) -> bool {
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] != clamp_work_suitability_bonus(expected[index], maxRank)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 验证可读快照中的所有请求字段是否等于经过范围限制的期望值。
 * @return 快照可读且每个非空请求字段都匹配时返回 `true`；未请求字段不参与验证。
 */
[[nodiscard]] inline auto verify_stat_edit(const PalStatValues& expected,
                                           const PalStatSnapshot& actual) -> bool {
    if (!actual.readable) {
        return false;
    }
    return (!expected.level.has_value() || actual.level == clamp_level(*expected.level)) &&
           (!expected.talentHp.has_value() ||
            actual.talentHp == clamp_talent(*expected.talentHp)) &&
           (!expected.talentShot.has_value() ||
            actual.talentShot == clamp_talent(*expected.talentShot)) &&
           (!expected.talentDefense.has_value() ||
            actual.talentDefense == clamp_talent(*expected.talentDefense)) &&
           (!expected.soulHpRank.has_value() ||
            actual.soulHpRank == clamp_soul_rank(*expected.soulHpRank)) &&
           (!expected.soulAttackRank.has_value() ||
            actual.soulAttackRank == clamp_soul_rank(*expected.soulAttackRank)) &&
           (!expected.soulDefenseRank.has_value() ||
            actual.soulDefenseRank == clamp_soul_rank(*expected.soulDefenseRank)) &&
           (!expected.soulWorkSpeedRank.has_value() ||
            actual.soulWorkSpeedRank == clamp_soul_rank(*expected.soulWorkSpeedRank)) &&
           (!expected.condensationStars.has_value() ||
            actual.condensationStars == clamp_condensation_stars(*expected.condensationStars,
                                                                 actual.condensationMaxStars)) &&
           (!expected.gender.has_value() ||
            (is_editable_gender(*expected.gender) && actual.gender == *expected.gender)) &&
           (!expected.workSuitabilityBonusRanks.has_value() ||
            verify_work_suitability_ranks(*expected.workSuitabilityBonusRanks,
                                          actual.workSuitabilityBonusRanks,
                                          actual.workSuitabilityMaxRank)) &&
           (!expected.friendshipRank.has_value() ||
            actual.friendshipRank == clamp_friendship_rank(*expected.friendshipRank));
}

/** @brief GUI 中可直接编辑的完整属性值。 */
struct PalStatEditableValues {
    int level{};                                      /**< 等级输入值。 */
    int talentHp{};                                   /**< HP 个体值输入值。 */
    int talentShot{};                                 /**< 攻击个体值输入值。 */
    int talentDefense{};                              /**< 防御个体值输入值。 */
    int soulHpRank{};                                 /**< 最大 HP 强化等级输入值。 */
    int soulAttackRank{};                             /**< 攻击强化等级输入值。 */
    int soulDefenseRank{};                            /**< 防御强化等级输入值。 */
    int soulWorkSpeedRank{};                          /**< 工作速度强化等级输入值。 */
    int condensationStars{};                          /**< 浓缩机 UI 星级输入值。 */
    PalGender gender{PalGender::none};                /**< 性别输入值。 */
    WorkSuitabilityRanks workSuitabilityBonusRanks{}; /**< 希望存档持有的绝对永久附加值。 */
    int friendshipRank{};                             /**< 亲密度等级输入值。 */
};

/**
 * @brief 以游戏快照为基线维护 GUI 草稿，并只生成真正发生变化的字段。
 * @details 草稿在明确同步到新目标前不会构造请求，避免初始占位值覆盖游戏中的真实属性。
 */
class PalStatEditDraft {
public:
    /** @brief 用目标的最新可读快照重置编辑基线。 */
    auto synchronize(const PalStatSnapshot& snapshot, const std::uint64_t targetGeneration) noexcept
        -> void {
        if (!snapshot.readable) {
            reset();
            return;
        }

        values_ = {
            .level = snapshot.level,
            .talentHp = snapshot.talentHp,
            .talentShot = snapshot.talentShot,
            .talentDefense = snapshot.talentDefense,
            .soulHpRank = snapshot.soulHpRank,
            .soulAttackRank = snapshot.soulAttackRank,
            .soulDefenseRank = snapshot.soulDefenseRank,
            .soulWorkSpeedRank = snapshot.soulWorkSpeedRank,
            .condensationStars = snapshot.condensationStars,
            .gender = snapshot.gender,
            .workSuitabilityBonusRanks = snapshot.workSuitabilityBonusRanks,
            .friendshipRank = snapshot.friendshipRank,
        };
        baseline_ = values_;
        baselineWorkSuitabilityBaseRanks_ = snapshot.workSuitabilityBaseRanks;
        workSuitabilityMaxRank_ = snapshot.workSuitabilityMaxRank;
        targetGeneration_ = targetGeneration;
        initialized_ = true;
    }

    /** @brief 清除草稿，使其在收到下一份可读快照前不能提交。 */
    auto reset() noexcept -> void {
        values_ = {};
        baseline_ = {};
        baselineWorkSuitabilityBaseRanks_ = {};
        workSuitabilityMaxRank_ = 0;
        targetGeneration_ = 0;
        initialized_ = false;
    }

    /** @return 可供 ImGui 控件直接修改的属性值。 */
    [[nodiscard]] auto values() noexcept -> PalStatEditableValues& {
        return values_;
    }
    /** @return 当前只读属性值。 */
    [[nodiscard]] auto values() const noexcept -> const PalStatEditableValues& {
        return values_;
    }

    /** @return 草稿是否已经由指定目标代数的可读快照初始化。 */
    [[nodiscard]] auto initialized_for(const std::uint64_t targetGeneration) const noexcept
        -> bool {
        return initialized_ && targetGeneration_ == targetGeneration;
    }

    /**
     * @brief 合并同一目标的新游戏快照，同时保留尚未提交成功的 GUI 改动。
     * @details 未修改字段跟随游戏更新；已修改字段保持草稿值，并把新游戏值作为下一次差量比较基线。
     */
    auto reconcile(const PalStatSnapshot& snapshot, const std::uint64_t targetGeneration) noexcept
        -> void {
        if (!snapshot.readable) {
            return;
        }
        if (!initialized_for(targetGeneration)) {
            synchronize(snapshot, targetGeneration);
            return;
        }

        const auto mergeField = [](auto& value, auto& baseline, const auto& gameValue) {
            const bool locallyChanged = value != baseline;
            baseline = gameValue;
            if (!locallyChanged || value == gameValue) {
                value = gameValue;
            }
        };
        mergeField(values_.level, baseline_.level, snapshot.level);
        mergeField(values_.talentHp, baseline_.talentHp, snapshot.talentHp);
        mergeField(values_.talentShot, baseline_.talentShot, snapshot.talentShot);
        mergeField(values_.talentDefense, baseline_.talentDefense, snapshot.talentDefense);
        mergeField(values_.soulHpRank, baseline_.soulHpRank, snapshot.soulHpRank);
        mergeField(values_.soulAttackRank, baseline_.soulAttackRank, snapshot.soulAttackRank);
        mergeField(values_.soulDefenseRank, baseline_.soulDefenseRank, snapshot.soulDefenseRank);
        mergeField(values_.soulWorkSpeedRank, baseline_.soulWorkSpeedRank,
                   snapshot.soulWorkSpeedRank);
        mergeField(values_.condensationStars, baseline_.condensationStars,
                   snapshot.condensationStars);
        mergeField(values_.gender, baseline_.gender, snapshot.gender);
        mergeField(values_.workSuitabilityBonusRanks, baseline_.workSuitabilityBonusRanks,
                   snapshot.workSuitabilityBonusRanks);
        baselineWorkSuitabilityBaseRanks_ = snapshot.workSuitabilityBaseRanks;
        workSuitabilityMaxRank_ = snapshot.workSuitabilityMaxRank;
        mergeField(values_.friendshipRank, baseline_.friendshipRank, snapshot.friendshipRank);
    }

    /** @return 只包含相对快照基线发生变化字段的世界绑定请求；无变化时返回空。 */
    [[nodiscard]] auto make_request(const std::uint64_t worldGeneration) const
        -> std::optional<PalStatEditRequest> {
        if (!initialized_) {
            return std::nullopt;
        }

        PalStatValues changedValues;
        if (values_.level != baseline_.level) {
            changedValues.level = values_.level;
        }
        if (values_.talentHp != baseline_.talentHp) {
            changedValues.talentHp = values_.talentHp;
        }
        if (values_.talentShot != baseline_.talentShot) {
            changedValues.talentShot = values_.talentShot;
        }
        if (values_.talentDefense != baseline_.talentDefense) {
            changedValues.talentDefense = values_.talentDefense;
        }
        if (values_.soulHpRank != baseline_.soulHpRank) {
            changedValues.soulHpRank = values_.soulHpRank;
        }
        if (values_.soulAttackRank != baseline_.soulAttackRank) {
            changedValues.soulAttackRank = values_.soulAttackRank;
        }
        if (values_.soulDefenseRank != baseline_.soulDefenseRank) {
            changedValues.soulDefenseRank = values_.soulDefenseRank;
        }
        if (values_.soulWorkSpeedRank != baseline_.soulWorkSpeedRank) {
            changedValues.soulWorkSpeedRank = values_.soulWorkSpeedRank;
        }
        if (values_.condensationStars != baseline_.condensationStars) {
            changedValues.condensationStars = values_.condensationStars;
        }
        if (values_.gender != baseline_.gender) {
            changedValues.gender = values_.gender;
        }
        if (values_.workSuitabilityBonusRanks != baseline_.workSuitabilityBonusRanks) {
            WorkSuitabilityRanks desiredBonuses{};
            for (std::size_t index{}; index < desiredBonuses.size(); ++index) {
                const int maxBonus = max_editable_work_suitability_bonus(
                    baselineWorkSuitabilityBaseRanks_[index],
                    baseline_.workSuitabilityBonusRanks[index], workSuitabilityMaxRank_);
                desiredBonuses[index] =
                    std::clamp(values_.workSuitabilityBonusRanks[index], 0, maxBonus);
            }
            changedValues.workSuitabilityBonusRanks = desiredBonuses;
        }
        if (values_.friendshipRank != baseline_.friendshipRank) {
            changedValues.friendshipRank = values_.friendshipRank;
        }
        if (!has_any_change(changedValues)) {
            return std::nullopt;
        }

        return PalStatEditRequest{
            .values = std::move(changedValues),
            .targetGeneration = targetGeneration_,
            .worldGeneration = worldGeneration,
        };
    }

    /** @return 仅包含等级、个体值、帕鲁之魂等基础属性的请求。 */
    [[nodiscard]] auto make_core_request(const std::uint64_t worldGeneration) const
        -> std::optional<PalStatEditRequest> {
        auto request = make_request(worldGeneration);
        if (!request.has_value()) {
            return std::nullopt;
        }
        request->values.workSuitabilityBonusRanks.reset();
        return has_core_stat_change(request->values) ? request : std::nullopt;
    }

    /** @return 仅包含工作适应性绝对永久附加值的请求。 */
    [[nodiscard]] auto make_work_suitability_request(const std::uint64_t worldGeneration) const
        -> std::optional<PalStatEditRequest> {
        const auto request = make_request(worldGeneration);
        if (!request.has_value() || !request->values.workSuitabilityBonusRanks.has_value()) {
            return std::nullopt;
        }
        PalStatValues workValues;
        workValues.workSuitabilityBonusRanks = request->values.workSuitabilityBonusRanks;
        return PalStatEditRequest{
            .values = std::move(workValues),
            .targetGeneration = request->targetGeneration,
            .worldGeneration = request->worldGeneration,
        };
    }

private:
    PalStatEditableValues values_{};   /**< 当前 GUI 草稿。 */
    PalStatEditableValues baseline_{}; /**< 最近一次可读游戏快照形成的比较基线。 */
    WorkSuitabilityRanks baselineWorkSuitabilityBaseRanks_{}; /**< 限制永久附加值的基础等级。 */
    int workSuitabilityMaxRank_{};     /**< 当前运行时允许的工作适应性合计上限。 */
    std::uint64_t targetGeneration_{}; /**< 草稿绑定的目标代数。 */
    bool initialized_{};               /**< 草稿是否已从真实游戏快照初始化。 */
};

/**
 * @brief 只保留最新一次属性编辑请求的线程安全交接槽。
 * @details 属性面板提交的是“期望最终状态”，旧请求可安全被新请求覆盖，从而为 UI 连续点击提供背压。
 */
class PalStatEditRequestSlot {
public:
    /** @brief 原子替换尚未消费的请求。 */
    auto submit(PalStatEditRequest request) -> void {
        const std::lock_guard lock(mutex_);
        request_ = std::move(request);
    }

    /** @return 取走最新请求；没有待处理请求时返回空。 */
    [[nodiscard]] auto consume() -> std::optional<PalStatEditRequest> {
        const std::lock_guard lock(mutex_);
        auto request = std::move(request_);
        request_.reset();
        return request;
    }

    /** @return 当前是否存在尚未消费的请求。 */
    [[nodiscard]] auto has_pending() const -> bool {
        const std::lock_guard lock(mutex_);
        return request_.has_value();
    }

    /** @brief 丢弃尚未消费的请求。 */
    auto clear() -> void {
        const std::lock_guard lock(mutex_);
        request_.reset();
    }

private:
    mutable std::mutex mutex_;                  /**< 保护最新请求槽。 */
    std::optional<PalStatEditRequest> request_; /**< 尚未由游戏线程消费的最新请求。 */
};

}  // namespace pal_stats
