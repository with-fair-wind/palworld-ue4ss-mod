/**
 * @file pal_stat_editor.hpp
 * @brief 与 Unreal 解耦的帕鲁属性编辑领域模型、范围 clamp 与线程安全请求队列。
 * @details 本文件只依赖标准库；具体游戏读写由 `PalStatGateway` 实现。
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

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
/** @brief 个体值上限（uint8 存储，突破游戏 100 上限）。 */
inline constexpr int kTalentMax = 255;
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
    PalStatValues values;               /**< 期望写入的属性值。 */
    std::uint64_t targetGeneration{};   /**< GUI 提交时观察到的已确认目标代数。 */
    std::uint64_t worldGeneration{};    /**< GUI 提交时观察到的世界代次。 */
};

/** @brief 从游戏读取到的当前属性值，供 GUI 显示。 */
struct PalStatSnapshot {
    int level{};          /**< 当前等级。 */
    int talentHp{};       /**< 当前个体值·HP。 */
    int talentShot{};     /**< 当前个体值·攻击。 */
    int talentDefense{};  /**< 当前个体值·防御。 */
    int friendshipRank{}; /**< 当前亲密度 rank。 */
    int friendshipPoint{}; /**< 当前亲密度原始点数。 */
    bool readable{};      /**< 是否已成功读取过一次（目标选中后）。 */
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
 * @brief 在线程安全 FIFO 中暂存 UI 提交的属性编辑请求。
 * @details 多生产者提交、唯一游戏线程按序消费；所有公开方法均在内部加锁。
 */
class PalStatEditQueue {
public:
    /** @brief 加锁后把请求追加到队尾。 */
    auto push(PalStatEditRequest request) -> void {
        const std::lock_guard lock(mutex_);
        requests_.push_back(request);
    }
    /** @return 加锁后从队首取出一个请求；队列为空时返回 `std::nullopt`。 */
    [[nodiscard]] auto try_pop() -> std::optional<PalStatEditRequest> {
        const std::lock_guard lock(mutex_);
        if (requests_.empty()) {
            return std::nullopt;
        }
        auto request = requests_.front();
        requests_.pop_front();
        return request;
    }
    /** @return 加锁后尚未处理的请求数量。 */
    [[nodiscard]] auto size() const -> std::size_t {
        const std::lock_guard lock(mutex_);
        return requests_.size();
    }
    /** @brief 加锁后丢弃全部待处理请求。 */
    auto clear() -> void {
        const std::lock_guard lock(mutex_);
        requests_.clear();
    }

private:
    mutable std::mutex mutex_;                   /**< 保护 `requests_` 的唯一互斥量。 */
    std::deque<PalStatEditRequest> requests_;    /**< 按提交顺序保存待执行请求。 */
};
}  // namespace pal_stats
