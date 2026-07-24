/**
 * @file skill_editor_service.hpp
 * @brief 定义与 Unreal 解耦的技能编辑领域模型、线程安全请求队列和验证/回滚服务。
 * @details 本文件只依赖标准库和值类型；具体游戏读写由 `ISkillGateway` 实现。
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

/** @brief 提供主动/被动技能编辑的纯领域接口和执行算法。 */
namespace skill_editor {
/**
 * @brief 将非拥有 `UObject*` 编码为整数的临时目标句柄。
 *
 * 使用前必须通过 ISkillGateway::is_valid 校验；句柄本身不延长 Unreal 对象的生命周期。
 */
using SkillTarget = std::uintptr_t;

/** @brief 一个可装备主动技能的数值标识与字符串标识。 */
struct ActiveSkill {
    /** @brief 用于比较、写入 EquipWaza 和验证的游戏数值标识。 */
    std::uint16_t value{};
    /** @brief 面向显示和目录查询的技能字符串标识。 */
    std::string id;

    /**
     * @brief 比较两个主动技能的全部字段是否相等。
     *
     * @details 第一个参数是左操作数，第二个参数是右操作数；两个参数均为只读输入。
     * @return 两个对象的 `value` 与 `id` 均相等时为 `true`，否则为 `false`。
     */
    friend auto operator==(const ActiveSkill&, const ActiveSkill&) -> bool = default;
};

/** @brief 从游戏读取到的被动技能列表和 EquipWaza 主动技能槽状态。 */
struct SkillState {
    /** @brief 当前拥有的被动技能标识。 */
    std::vector<std::string> passiveIds;
    /** @brief 当前 EquipWaza 槽位顺序中的主动技能。 */
    std::vector<ActiveSkill> activeSkills;
};

/** @brief 要编辑的技能类别。 */
enum class SkillKind {
    passive, /**< 被动技能列表。 */
    active,  /**< EquipWaza 主动技能槽。 */
};

/** @brief 对选定技能类别执行的编辑动作。 */
enum class SkillEditOperation {
    add,     /**< 新增一项技能。 */
    replace, /**< 用新技能替换已有技能。 */
    remove,  /**< 移除已有技能。 */
};

/** @brief 由 UI 提交、等待游戏线程执行的一次技能编辑请求。 */
struct SkillEditRequest {
    /** @brief 待编辑 Pal 的临时句柄；执行前必须校验。 */
    SkillTarget target{};
    /** @brief GUI 提交请求时观察到的已确认目标代数。 */
    std::uint64_t targetGeneration{};
    /** @brief 决定请求操作被动列表还是主动槽位。 */
    SkillKind kind{};
    /** @brief 决定新增、替换或移除的编辑语义。 */
    SkillEditOperation operation{};
    /** @brief 仅当 `kind` 为 active 时生效的 EquipWaza 槽位索引。 */
    std::size_t activeSlot{};
    /** @brief 仅当 `kind` 为 passive 且 `operation` 为 replace/remove 时生效的原被动技能标识。 */
    std::string oldPassiveId;
    /** @brief 仅当 `kind` 为 passive 且 `operation` 为 add/replace 时生效的新被动技能标识。 */
    std::string newPassiveId;
    /** @brief 仅当 `kind` 为 active 且 `operation` 为 add/replace 时生效的新主动技能。 */
    std::optional<ActiveSkill> newActiveSkill;
};

/** @brief 技能编辑执行后可供 UI 展示的终态。 */
enum class SkillEditStatus {
    succeeded,      /**< 写入后重读验证与期望状态一致。 */
    invalidTarget,  /**< 目标为空或已失效，尚未读取或修改游戏状态。 */
    rejected,       /**< 请求在写入前未通过领域规则验证。 */
    failed,         /**< 游戏未接受写入，且无需或未尝试回滚。 */
    rolledBack,     /**< 写入验证失败，但重读确认原始状态已恢复。 */
    rollbackFailed, /**< 写入和回滚后均未能恢复期望的原始状态。 */
};

/** @brief 一次编辑尝试的状态、实际游戏快照和面向 UI 的说明。 */
struct SkillEditResult {
    /** @brief 编辑或回滚后重新读取到的实际游戏状态。 */
    SkillEditStatus status{};
    /** @brief 可获得的最新实际游戏状态。 */
    SkillState state;
    /** @brief 可直接显示给 UI 的结果消息。 */
    std::string message;
};

/**
 * @brief 在线程安全 FIFO 中暂存 UI 提交的编辑请求。
 *
 * 支持多个生产者提交请求，并由唯一游戏线程消费者按提交顺序取出执行；所有公开方法均在内部加锁。
 */
class SkillEditQueue {
public:
    /**
     * @brief 加锁后将请求追加到 FIFO 队尾。
     *
     * @param[in] request 要移入队列、等待游戏线程执行的编辑请求。
     */
    auto push(SkillEditRequest request) -> void {
        const std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
    }

    /**
     * @brief 加锁后从 FIFO 队首取出一个请求。
     *
     * @return 队列非空时返回最早提交的请求；队列为空时返回 std::nullopt。
     */
    [[nodiscard]] auto try_pop() -> std::optional<SkillEditRequest> {
        const std::lock_guard lock(mutex_);
        if (requests_.empty()) {
            return std::nullopt;
        }

        auto request = std::move(requests_.front());
        requests_.pop_front();
        return request;
    }

    /**
     * @brief 加锁后返回尚未由游戏线程执行的请求数量。
     *
     * @return 当前 FIFO 队列中等待执行的请求数。
     */
    [[nodiscard]] auto size() const -> std::size_t {
        const std::lock_guard lock(mutex_);
        return requests_.size();
    }

    /**
     * @brief 加锁后丢弃全部待处理请求。
     * @return 被丢弃的请求数量。
     */
    auto clear() -> std::size_t {
        const std::lock_guard lock(mutex_);
        const auto discarded = requests_.size();
        requests_.clear();
        return discarded;
    }

private:
    mutable std::mutex mutex_;              /**< 保护 `requests_` 的唯一互斥量。 */
    std::deque<SkillEditRequest> requests_; /**< 按提交顺序保存尚未由游戏线程执行的请求。 */
};

/** @brief 将领域服务与 Unreal 反射及实际游戏对象隔离的游戏访问网关。 */
class ISkillGateway {
public:
    /** @brief 确保通过接口销毁派生网关时正确析构。 */
    virtual ~ISkillGateway() = default;

    /**
     * @brief 校验临时目标句柄当前是否仍指向可操作的游戏对象。
     *
     * @param[in] target 待校验的非拥有临时目标句柄。
     * @retval true `target` 当前可安全读取或修改。
     * @retval false `target` 为空、已失效或不再可操作。
     */
    [[nodiscard]] virtual auto is_valid(SkillTarget target) const -> bool = 0;
    /**
     * @brief 读取目标的实际游戏值。
     *
     * @param[in] target 已通过 is_valid 校验的临时目标句柄。
     * @return 从游戏读取到的实际被动技能和主动技能槽状态。
     */
    virtual auto read_state(SkillTarget target) -> SkillState = 0;
    /**
     * @brief 请求添加被动技能。
     *
     * @param[in] target 已通过 is_valid 校验的临时目标句柄。
     * @param[in] id 要添加的被动技能标识。
     * @retval true 反射调用已成功发起，但游戏是否接受修改仍须重读验证。
     * @retval false 反射调用未能发起；调用方仍应按需要重读实际状态。
     */
    virtual auto add_passive(SkillTarget target, std::string_view id) -> bool = 0;
    /**
     * @brief 请求移除被动技能。
     *
     * @param[in] target 已通过 is_valid 校验的临时目标句柄。
     * @param[in] id 要移除的被动技能标识。
     * @retval true 反射调用已成功发起，但游戏是否接受修改仍须重读验证。
     * @retval false 反射调用未能发起；调用方仍应按需要重读实际状态。
     */
    virtual auto remove_passive(SkillTarget target, std::string_view id) -> bool = 0;
    /**
     * @brief 以给定 EquipWaza 槽位顺序重写主动技能。
     *
     * @param[in] target 已通过 is_valid 校验的临时目标句柄。
     * @param[in] skills 最多三项的目标主动技能序列；输入顺序就是 EquipWaza 槽位顺序。
     * @retval true 反射调用已成功发起，但游戏是否接受修改仍须重读验证。
     * @retval false 反射调用未能发起；调用方仍应按需要重读实际状态。
     *
     * 实现可能先清空再逐项重写。
     */
    virtual auto rewrite_active(SkillTarget target, std::span<const ActiveSkill> skills)
        -> bool = 0;
};

/** @brief 不对外暴露的请求验证、状态比较与执行辅助算法。 */
namespace detail {
/** @brief 判断被动技能列表是否包含给定标识。 */
[[nodiscard]] inline auto contains_passive(const std::vector<std::string>& passives,
                                           const std::string_view id) -> bool {
    return std::ranges::find(passives, id) != passives.end();
}

/** @brief 比较两份被动技能列表；忽略顺序但保留每个标识的重复次数。 */
[[nodiscard]] inline auto same_passives(const std::vector<std::string>& left,
                                        const std::vector<std::string>& right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    return std::unordered_multiset<std::string>(left.begin(), left.end()) ==
           std::unordered_multiset<std::string>(right.begin(), right.end());
}

/** @brief 组装包含移动后状态和消息的编辑结果。 */
[[nodiscard]] inline auto result(const SkillEditStatus status, SkillState state,
                                 std::string message) -> SkillEditResult {
    return {.status = status, .state = std::move(state), .message = std::move(message)};
}

/**
 * @brief 执行被动技能编辑的验证、写入、重读与回滚状态机。
 *
 * 先验证请求和原始状态，再发起写入并重读实际状态；替换失败时尝试恢复原技能。
 * 重读确认恢复则返回 rolledBack，否则返回 rollbackFailed。
 */
[[nodiscard]] inline auto execute_passive(ISkillGateway& gateway, const SkillEditRequest& request,
                                          const SkillState& original) -> SkillEditResult {
    const auto reject = [&](std::string message) {
        return result(SkillEditStatus::rejected, original, std::move(message));
    };

    if (request.operation == SkillEditOperation::add) {
        if (request.newPassiveId.empty()) {
            return reject("Passive skill ID is empty");
        }
        if (original.passiveIds.size() >= 4) {
            return reject("A Pal cannot have more than four passive skills");
        }
        if (contains_passive(original.passiveIds, request.newPassiveId)) {
            return reject("Passive skill is already present");
        }

        gateway.add_passive(request.target, request.newPassiveId);
        auto actual = gateway.read_state(request.target);
        if (contains_passive(actual.passiveIds, request.newPassiveId)) {
            return result(SkillEditStatus::succeeded, std::move(actual), "Passive skill added");
        }
        return result(SkillEditStatus::failed, std::move(actual),
                      "Game rejected passive skill add");
    }

    if (request.oldPassiveId.empty() ||
        !contains_passive(original.passiveIds, request.oldPassiveId)) {
        return reject("Original passive skill is no longer present");
    }

    if (request.operation == SkillEditOperation::remove) {
        gateway.remove_passive(request.target, request.oldPassiveId);
        auto actual = gateway.read_state(request.target);
        if (!contains_passive(actual.passiveIds, request.oldPassiveId)) {
            return result(SkillEditStatus::succeeded, std::move(actual), "Passive skill removed");
        }
        return result(SkillEditStatus::failed, std::move(actual),
                      "Game rejected passive skill remove");
    }

    if (request.newPassiveId.empty() || request.newPassiveId == request.oldPassiveId ||
        contains_passive(original.passiveIds, request.newPassiveId)) {
        return reject("Replacement passive skill is invalid or already present");
    }

    gateway.remove_passive(request.target, request.oldPassiveId);
    gateway.add_passive(request.target, request.newPassiveId);
    auto actual = gateway.read_state(request.target);
    if (!contains_passive(actual.passiveIds, request.oldPassiveId) &&
        contains_passive(actual.passiveIds, request.newPassiveId)) {
        return result(SkillEditStatus::succeeded, std::move(actual), "Passive skill replaced");
    }

    if (contains_passive(actual.passiveIds, request.newPassiveId)) {
        gateway.remove_passive(request.target, request.newPassiveId);
    }
    if (!contains_passive(actual.passiveIds, request.oldPassiveId)) {
        gateway.add_passive(request.target, request.oldPassiveId);
    }

    auto rolledBack = gateway.read_state(request.target);
    if (same_passives(rolledBack.passiveIds, original.passiveIds)) {
        return result(SkillEditStatus::rolledBack, std::move(rolledBack),
                      "Replace failed; original restored");
    }
    return result(SkillEditStatus::rollbackFailed, std::move(rolledBack),
                  "Replace and rollback both failed");
}

/** @brief 判断主动技能列表中是否存在给定数值标识。 */
[[nodiscard]] inline auto contains_active(const std::vector<ActiveSkill>& skills,
                                          const std::uint16_t value) -> bool {
    return std::ranges::any_of(skills,
                               [value](const ActiveSkill& skill) { return skill.value == value; });
}

/** @brief 仅按主动技能数值及其槽位顺序比较两条 EquipWaza 序列。 */
[[nodiscard]] inline auto same_active_sequence(const std::vector<ActiveSkill>& left,
                                               const std::vector<ActiveSkill>& right) -> bool {
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](const ActiveSkill& lhs, const ActiveSkill& rhs) {
               return lhs.value == rhs.value;
           });
}

/**
 * @brief 执行主动技能编辑的验证、写入、重读与回滚状态机。
 *
 * 前置请求或槽位验证失败时直接返回 rejected，不触发回滚。仅当已经调用 rewrite_active，
 * 随后重读验证失败时，才重写原始序列并重读。恢复成功返回 rolledBack，
 * 恢复失败返回 rollbackFailed。
 */
[[nodiscard]] inline auto execute_active(ISkillGateway& gateway, const SkillEditRequest& request,
                                         const SkillState& original) -> SkillEditResult {
    const auto reject = [&](std::string message) {
        return result(SkillEditStatus::rejected, original, std::move(message));
    };

    if (request.activeSlot >= 3 || original.activeSkills.size() > 3) {
        return reject("Active skill slot is outside the three EquipWaza slots");
    }

    auto desired = original.activeSkills;
    if (request.operation == SkillEditOperation::add) {
        if (!request.newActiveSkill.has_value() || request.activeSlot != desired.size() ||
            desired.size() >= 3) {
            return reject("Active skill can only be added to the first empty trailing slot");
        }
        if (contains_active(desired, request.newActiveSkill->value)) {
            return reject("Active skill is already equipped");
        }
        desired.push_back(*request.newActiveSkill);
    } else if (request.operation == SkillEditOperation::replace) {
        if (!request.newActiveSkill.has_value() || request.activeSlot >= desired.size()) {
            return reject("Active skill replacement slot is empty");
        }
        if (contains_active(desired, request.newActiveSkill->value)) {
            return reject("Active skill is already equipped");
        }
        desired[request.activeSlot] = *request.newActiveSkill;
    } else {
        if (request.activeSlot >= desired.size()) {
            return reject("Active skill removal slot is empty");
        }
        desired.erase(desired.begin() + static_cast<std::ptrdiff_t>(request.activeSlot));
    }

    gateway.rewrite_active(request.target, desired);
    auto actual = gateway.read_state(request.target);
    if (same_active_sequence(actual.activeSkills, desired)) {
        return result(SkillEditStatus::succeeded, std::move(actual), "Active skill slots updated");
    }

    gateway.rewrite_active(request.target, original.activeSkills);
    auto rolledBack = gateway.read_state(request.target);
    if (same_active_sequence(rolledBack.activeSkills, original.activeSkills)) {
        return result(SkillEditStatus::rolledBack, std::move(rolledBack),
                      "Active edit failed; original slots restored");
    }
    return result(SkillEditStatus::rollbackFailed, std::move(rolledBack),
                  "Active edit and rollback both failed");
}
}  // namespace detail

/**
 * @brief 执行一次已排队的技能编辑请求。
 *
 * 先拒绝空或失效目标，再读取原始状态并按 SkillKind 分派。
 * 返回值始终包含可获得的最新实际状态和面向 UI 的消息。
 *
 * @param[in] gateway 提供目标校验、游戏状态读取和技能写入的网关。
 * @param[in] request 要验证并执行的技能编辑请求。
 * @return 包含执行状态、可获得的最新实际状态和面向 UI 消息的结果。
 *
 * @warning 若网关实现涉及 Unreal 反射，必须在游戏线程调用此函数。
 */
[[nodiscard]] inline auto execute_skill_edit(ISkillGateway& gateway,
                                             const SkillEditRequest& request) -> SkillEditResult {
    if (request.target == 0 || !gateway.is_valid(request.target)) {
        return {.status = SkillEditStatus::invalidTarget,
                .message = "Target Pal is no longer valid"};
    }

    const auto original = gateway.read_state(request.target);
    if (request.kind == SkillKind::passive) {
        return detail::execute_passive(gateway, request, original);
    }
    return detail::execute_active(gateway, request, original);
}
}  // namespace skill_editor
