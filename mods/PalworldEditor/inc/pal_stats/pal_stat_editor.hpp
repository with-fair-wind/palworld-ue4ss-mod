/**
 * @file pal_stat_editor.hpp
 * @brief 与 Unreal 解耦的帕鲁属性编辑领域模型、差量草稿与线程安全请求槽。
 * @details 本文件只依赖标准库；具体游戏读写由 `PalStatGateway` 实现。
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

/** @brief 提供帕鲁等级/个体值/亲密度编辑的纯值领域模型。 */
namespace pal_stats {
/** @brief 由非拥有 `UObject*` 编码的临时帕鲁目标句柄；使用前必须由网关校验。 */
using PalStatTarget = std::uintptr_t;

/** @brief 等级下限（不超过游戏满级，避免经验表空段）。 */
inline constexpr int kLevelMin = 1;
/** @brief 等级上限。 */
inline constexpr int kLevelMax = 80;
/** @brief 个体值下限。 */
inline constexpr int kTalentMin = 0;
/** @brief 普通个体值上限；拒绝生成超出游戏正常范围的存档数据。 */
inline constexpr int kTalentMax = 100;
/** @brief 亲密度 rank 下限。 */
inline constexpr int kFriendshipRankMin = 0;
/** @brief 亲密度 rank 上限。 */
inline constexpr int kFriendshipRankMax = 10;

/**
 * @brief 期望写入的属性值；空 `optional` 表示不改该项。
 */
struct PalStatValues {
    std::optional<int> level;          /**< 等级，clamp 到 `[1, 80]`。 */
    std::optional<int> talentHp;       /**< 个体值·HP（`Talent_HP`）。 */
    std::optional<int> talentShot;     /**< 个体值·攻击（`Talent_Shot`，远程）。 */
    std::optional<int> talentDefense;  /**< 个体值·防御（`Talent_Defense`）。 */
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
    int level{};           /**< 当前等级。 */
    int talentHp{};        /**< 当前个体值·HP。 */
    int talentShot{};      /**< 当前个体值·攻击。 */
    int talentDefense{};   /**< 当前个体值·防御。 */
    int friendshipRank{};  /**< 当前亲密度 rank。 */
    int friendshipPoint{}; /**< 当前亲密度原始点数。 */
    bool readable{};       /**< 是否已成功读取过一次（目标选中后）。 */
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
/** @brief 把亲密度 rank 限制到 `[kFriendshipRankMin, kFriendshipRankMax]`。 */
[[nodiscard]] inline auto clamp_friendship_rank(const int value) -> int {
    return std::clamp(value, kFriendshipRankMin, kFriendshipRankMax);
}
/** @return `values` 是否至少设置了一个待写字段。 */
[[nodiscard]] inline auto has_any_change(const PalStatValues& values) -> bool {
    return values.level.has_value() || values.talentHp.has_value() ||
           values.talentShot.has_value() || values.talentDefense.has_value() ||
           values.friendshipRank.has_value();
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
           (!expected.friendshipRank.has_value() ||
            actual.friendshipRank == clamp_friendship_rank(*expected.friendshipRank));
}

/** @brief GUI 中可直接编辑的完整属性值。 */
struct PalStatEditableValues {
    int level{};          /**< 等级输入值。 */
    int talentHp{};       /**< HP 个体值输入值。 */
    int talentShot{};     /**< 攻击个体值输入值。 */
    int talentDefense{};  /**< 防御个体值输入值。 */
    int friendshipRank{}; /**< 亲密度等级输入值。 */
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
            .friendshipRank = snapshot.friendshipRank,
        };
        baseline_ = values_;
        targetGeneration_ = targetGeneration;
        initialized_ = true;
    }

    /** @brief 清除草稿，使其在收到下一份可读快照前不能提交。 */
    auto reset() noexcept -> void {
        values_ = {};
        baseline_ = {};
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

        const auto mergeField = [](int& value, int& baseline, const int gameValue) {
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

private:
    PalStatEditableValues values_{};   /**< 当前 GUI 草稿。 */
    PalStatEditableValues baseline_{}; /**< 最近一次可读游戏快照形成的比较基线。 */
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
