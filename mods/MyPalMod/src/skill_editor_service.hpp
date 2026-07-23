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

namespace skill_editor
{
using SkillTarget = std::uintptr_t;

struct ActiveSkill
{
    std::uint16_t value{};
    std::string id;

    friend auto operator==(const ActiveSkill&, const ActiveSkill&) -> bool = default;
};

struct SkillState
{
    std::vector<std::string> passiveIds;
    std::vector<ActiveSkill> activeSkills;
};

enum class SkillKind
{
    passive,
    active,
};

enum class SkillEditOperation
{
    add,
    replace,
    remove,
};

struct SkillEditRequest
{
    SkillTarget target{};
    SkillKind kind{};
    SkillEditOperation operation{};
    std::size_t activeSlot{};
    std::string oldPassiveId;
    std::string newPassiveId;
    std::optional<ActiveSkill> newActiveSkill;
};

enum class SkillEditStatus
{
    succeeded,
    invalidTarget,
    rejected,
    failed,
    rolledBack,
    rollbackFailed,
};

struct SkillEditResult
{
    SkillEditStatus status{};
    SkillState state;
    std::string message;
};

class SkillEditQueue
{
public:
    auto push(SkillEditRequest request) -> void
    {
        const std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
    }

    [[nodiscard]] auto try_pop() -> std::optional<SkillEditRequest>
    {
        const std::lock_guard lock(mutex_);
        if (requests_.empty())
        {
            return std::nullopt;
        }

        auto request = std::move(requests_.front());
        requests_.pop_front();
        return request;
    }

    [[nodiscard]] auto size() const -> std::size_t
    {
        const std::lock_guard lock(mutex_);
        return requests_.size();
    }

    [[nodiscard]] auto contains_target(const SkillTarget target) const -> bool
    {
        const std::lock_guard lock(mutex_);
        return std::ranges::any_of(
            requests_, [target](const SkillEditRequest& request) { return request.target == target; });
    }

private:
    mutable std::mutex mutex_;
    std::deque<SkillEditRequest> requests_;
};

class ISkillGateway
{
public:
    virtual ~ISkillGateway() = default;

    [[nodiscard]] virtual auto is_valid(SkillTarget target) const -> bool = 0;
    virtual auto read_state(SkillTarget target) -> SkillState = 0;
    virtual auto add_passive(SkillTarget target, std::string_view id) -> bool = 0;
    virtual auto remove_passive(SkillTarget target, std::string_view id) -> bool = 0;
    virtual auto rewrite_active(SkillTarget target, std::span<const ActiveSkill> skills) -> bool = 0;
};

namespace detail
{
[[nodiscard]] inline auto contains_passive(
    const std::vector<std::string>& passives,
    const std::string_view id) -> bool
{
    return std::ranges::find(passives, id) != passives.end();
}

[[nodiscard]] inline auto same_passives(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right) -> bool
{
    if (left.size() != right.size())
    {
        return false;
    }
    return std::unordered_multiset<std::string>(left.begin(), left.end()) ==
           std::unordered_multiset<std::string>(right.begin(), right.end());
}

[[nodiscard]] inline auto result(
    const SkillEditStatus status,
    SkillState state,
    std::string message) -> SkillEditResult
{
    return {.status = status, .state = std::move(state), .message = std::move(message)};
}

[[nodiscard]] inline auto execute_passive(
    ISkillGateway& gateway,
    const SkillEditRequest& request,
    const SkillState& original) -> SkillEditResult
{
    const auto reject = [&](std::string message)
    {
        return result(SkillEditStatus::rejected, original, std::move(message));
    };

    if (request.operation == SkillEditOperation::add)
    {
        if (request.newPassiveId.empty())
        {
            return reject("Passive skill ID is empty");
        }
        if (original.passiveIds.size() >= 4)
        {
            return reject("A Pal cannot have more than four passive skills");
        }
        if (contains_passive(original.passiveIds, request.newPassiveId))
        {
            return reject("Passive skill is already present");
        }

        gateway.add_passive(request.target, request.newPassiveId);
        auto actual = gateway.read_state(request.target);
        if (contains_passive(actual.passiveIds, request.newPassiveId))
        {
            return result(SkillEditStatus::succeeded, std::move(actual), "Passive skill added");
        }
        return result(SkillEditStatus::failed, std::move(actual), "Game rejected passive skill add");
    }

    if (request.oldPassiveId.empty() || !contains_passive(original.passiveIds, request.oldPassiveId))
    {
        return reject("Original passive skill is no longer present");
    }

    if (request.operation == SkillEditOperation::remove)
    {
        gateway.remove_passive(request.target, request.oldPassiveId);
        auto actual = gateway.read_state(request.target);
        if (!contains_passive(actual.passiveIds, request.oldPassiveId))
        {
            return result(SkillEditStatus::succeeded, std::move(actual), "Passive skill removed");
        }
        return result(SkillEditStatus::failed, std::move(actual), "Game rejected passive skill remove");
    }

    if (request.newPassiveId.empty() || request.newPassiveId == request.oldPassiveId ||
        contains_passive(original.passiveIds, request.newPassiveId))
    {
        return reject("Replacement passive skill is invalid or already present");
    }

    gateway.remove_passive(request.target, request.oldPassiveId);
    gateway.add_passive(request.target, request.newPassiveId);
    auto actual = gateway.read_state(request.target);
    if (!contains_passive(actual.passiveIds, request.oldPassiveId) &&
        contains_passive(actual.passiveIds, request.newPassiveId))
    {
        return result(SkillEditStatus::succeeded, std::move(actual), "Passive skill replaced");
    }

    if (contains_passive(actual.passiveIds, request.newPassiveId))
    {
        gateway.remove_passive(request.target, request.newPassiveId);
    }
    if (!contains_passive(actual.passiveIds, request.oldPassiveId))
    {
        gateway.add_passive(request.target, request.oldPassiveId);
    }

    auto rolledBack = gateway.read_state(request.target);
    if (same_passives(rolledBack.passiveIds, original.passiveIds))
    {
        return result(SkillEditStatus::rolledBack, std::move(rolledBack), "Replace failed; original restored");
    }
    return result(SkillEditStatus::rollbackFailed, std::move(rolledBack), "Replace and rollback both failed");
}

[[nodiscard]] inline auto contains_active(
    const std::vector<ActiveSkill>& skills,
    const std::uint16_t value) -> bool
{
    return std::ranges::any_of(skills, [value](const ActiveSkill& skill) { return skill.value == value; });
}

[[nodiscard]] inline auto same_active_sequence(
    const std::vector<ActiveSkill>& left,
    const std::vector<ActiveSkill>& right) -> bool
{
    return left.size() == right.size() &&
           std::ranges::equal(
               left,
               right,
               [](const ActiveSkill& lhs, const ActiveSkill& rhs) { return lhs.value == rhs.value; });
}

[[nodiscard]] inline auto execute_active(
    ISkillGateway& gateway,
    const SkillEditRequest& request,
    const SkillState& original) -> SkillEditResult
{
    const auto reject = [&](std::string message)
    {
        return result(SkillEditStatus::rejected, original, std::move(message));
    };

    if (request.activeSlot >= 3 || original.activeSkills.size() > 3)
    {
        return reject("Active skill slot is outside the three EquipWaza slots");
    }

    auto desired = original.activeSkills;
    if (request.operation == SkillEditOperation::add)
    {
        if (!request.newActiveSkill.has_value() || request.activeSlot != desired.size() || desired.size() >= 3)
        {
            return reject("Active skill can only be added to the first empty trailing slot");
        }
        if (contains_active(desired, request.newActiveSkill->value))
        {
            return reject("Active skill is already equipped");
        }
        desired.push_back(*request.newActiveSkill);
    }
    else if (request.operation == SkillEditOperation::replace)
    {
        if (!request.newActiveSkill.has_value() || request.activeSlot >= desired.size())
        {
            return reject("Active skill replacement slot is empty");
        }
        if (contains_active(desired, request.newActiveSkill->value))
        {
            return reject("Active skill is already equipped");
        }
        desired[request.activeSlot] = *request.newActiveSkill;
    }
    else
    {
        if (request.activeSlot >= desired.size())
        {
            return reject("Active skill removal slot is empty");
        }
        desired.erase(desired.begin() + static_cast<std::ptrdiff_t>(request.activeSlot));
    }

    gateway.rewrite_active(request.target, desired);
    auto actual = gateway.read_state(request.target);
    if (same_active_sequence(actual.activeSkills, desired))
    {
        return result(SkillEditStatus::succeeded, std::move(actual), "Active skill slots updated");
    }

    gateway.rewrite_active(request.target, original.activeSkills);
    auto rolledBack = gateway.read_state(request.target);
    if (same_active_sequence(rolledBack.activeSkills, original.activeSkills))
    {
        return result(SkillEditStatus::rolledBack, std::move(rolledBack), "Active edit failed; original slots restored");
    }
    return result(SkillEditStatus::rollbackFailed, std::move(rolledBack), "Active edit and rollback both failed");
}
}

[[nodiscard]] inline auto execute_skill_edit(
    ISkillGateway& gateway,
    const SkillEditRequest& request) -> SkillEditResult
{
    if (request.target == 0 || !gateway.is_valid(request.target))
    {
        return {.status = SkillEditStatus::invalidTarget, .message = "Target Pal is no longer valid"};
    }

    const auto original = gateway.read_state(request.target);
    if (request.kind == SkillKind::passive)
    {
        return detail::execute_passive(gateway, request, original);
    }
    return detail::execute_active(gateway, request, original);
}
}
