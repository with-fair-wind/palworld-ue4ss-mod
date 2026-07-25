#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace base_resource_sharing {
struct GuidKey {
    std::array<std::uint32_t, 4> words{};

    [[nodiscard]] auto valid() const noexcept -> bool {
        return std::ranges::any_of(words, [](const auto word) { return word != 0; });
    }

    auto operator<=>(const GuidKey&) const = default;
};

enum class ContainerKind : std::uint8_t { normal, food, player, other };

struct ContainerDescriptor {
    GuidKey baseId;
    GuidKey groupId;
    GuidKey containerId;
    ContainerKind kind{ContainerKind::other};
    bool currentBase{};
};

struct ResourceUnionPlan {
    std::vector<ContainerDescriptor> ordered;
    std::size_t baseCount{};
    std::string error;
};

[[nodiscard]] inline auto make_resource_union_plan(
    const std::span<const ContainerDescriptor> containers, const GuidKey& currentGuild)
    -> ResourceUnionPlan {
    ResourceUnionPlan result;
    if (!currentGuild.valid()) {
        result.error = "当前玩家的公会标识无效。";
        return result;
    }

    for (const auto& descriptor : containers) {
        if (descriptor.kind == ContainerKind::normal && descriptor.baseId.valid() &&
            descriptor.containerId.valid() && descriptor.groupId == currentGuild) {
            result.ordered.push_back(descriptor);
        }
    }

    std::ranges::sort(result.ordered, [](const auto& left, const auto& right) {
        if (left.currentBase != right.currentBase) {
            return left.currentBase;
        }
        if (left.baseId != right.baseId) {
            return left.baseId < right.baseId;
        }
        return left.containerId < right.containerId;
    });
    const auto duplicate = std::ranges::unique(
        result.ordered, {}, [](const auto& descriptor) { return descriptor.containerId; });
    result.ordered.erase(duplicate.begin(), duplicate.end());

    std::vector<GuidKey> bases;
    for (const auto& descriptor : result.ordered) {
        if (std::ranges::find(bases, descriptor.baseId) == bases.end()) {
            bases.push_back(descriptor.baseId);
        }
    }
    result.baseCount = bases.size();
    if (result.ordered.empty()) {
        result.error = "未发现当前公会已加载的普通据点资源容器。";
    }
    return result;
}

enum class ResourceOperation : std::uint8_t { crafting, building, repair };

[[nodiscard]] constexpr auto operation_index(const ResourceOperation operation) noexcept
    -> std::size_t {
    return static_cast<std::size_t>(operation);
}

struct CapabilityState {
    bool previewReady{};
    bool consumeReady{};
    std::string error;

    [[nodiscard]] auto available() const noexcept -> bool {
        return previewReady && consumeReady;
    }
};

class RuntimeState {
public:
    auto set_preference(const bool enabled) noexcept -> void {
        enabled_ = enabled;
    }

    auto begin_world_transition(const std::uint64_t generation) -> void {
        generation_ = generation;
        accessible_ = false;
        capabilities_ = {};
    }

    auto finish_world_transition(const std::uint64_t generation) -> void {
        generation_ = generation;
        accessible_ = true;
        capabilities_ = {};
    }

    auto set_capability(const ResourceOperation operation, CapabilityState capability) -> void {
        capabilities_[operation_index(operation)] = std::move(capability);
    }

    [[nodiscard]] auto can_extend(const ResourceOperation operation,
                                  const std::uint64_t generation) const -> bool {
        return enabled_ && accessible_ && generation == generation_ &&
               capabilities_[operation_index(operation)].available();
    }

    [[nodiscard]] auto generation() const noexcept -> std::uint64_t {
        return generation_;
    }

    [[nodiscard]] auto enabled() const noexcept -> bool {
        return enabled_;
    }

    [[nodiscard]] auto accessible() const noexcept -> bool {
        return accessible_;
    }

    [[nodiscard]] auto capability(const ResourceOperation operation) const
        -> const CapabilityState& {
        return capabilities_[operation_index(operation)];
    }

private:
    bool enabled_{};
    bool accessible_{};
    std::uint64_t generation_{};
    std::array<CapabilityState, 3> capabilities_{};
};

struct ValidationResult {
    std::string error;
};

[[nodiscard]] inline auto validate_live_container_resolution(
    const std::span<const ContainerDescriptor> registered, const std::span<const GuidKey> resolved)
    -> ValidationResult {
    std::vector<GuidKey> expected;
    for (const auto& descriptor : registered) {
        if (descriptor.containerId.valid() &&
            std::ranges::find(expected, descriptor.containerId) == expected.end()) {
            expected.push_back(descriptor.containerId);
        }
    }

    std::vector<GuidKey> actual;
    for (const auto& id : resolved) {
        if (id.valid() && std::ranges::find(actual, id) == actual.end()) {
            actual.push_back(id);
        }
    }

    const bool complete =
        expected.size() == actual.size() && std::ranges::all_of(expected, [&](const auto& id) {
            return std::ranges::find(actual, id) != actual.end();
        });
    if (complete && !expected.empty()) {
        return {};
    }
    return {.error = "仅解析到 " + std::to_string(actual.size()) + "/" +
                     std::to_string(expected.size()) + " 个已登记据点资源容器。"};
}

struct ArrayPatchLedger {
    std::wstring objectFullName;
    std::vector<GuidKey> original;
    std::vector<GuidKey> appended;
    bool helperArray{};
};

[[nodiscard]] inline auto verify_restoration_sequence(const std::span<const GuidKey> original,
                                                      const std::span<const GuidKey> current,
                                                      const std::span<const GuidKey> appended)
    -> bool {
    return current.size() == original.size() + appended.size() &&
           std::ranges::equal(current.first(original.size()), original) &&
           std::ranges::equal(current.last(appended.size()), appended);
}

[[nodiscard]] inline auto missing_union_tail(const std::span<const GuidKey> existing,
                                             const std::span<const GuidKey> globalPlan)
    -> std::vector<GuidKey> {
    std::vector<GuidKey> missing;
    for (const auto& id : globalPlan) {
        if (std::ranges::find(existing, id) == existing.end() &&
            std::ranges::find(missing, id) == missing.end()) {
            missing.push_back(id);
        }
    }
    return missing;
}

class RequestGuard {
public:
    [[nodiscard]] auto try_enter(const ResourceOperation operation,
                                 const std::uint64_t generation) noexcept -> bool {
        if (depth_ != 0) {
            return false;
        }
        operation_ = operation;
        generation_ = generation;
        depth_ = 1;
        return true;
    }

    auto leave(const ResourceOperation operation, const std::uint64_t generation) noexcept -> void {
        if (depth_ == 1 && operation_ == operation && generation_ == generation) {
            reset();
        }
    }

    auto reset() noexcept -> void {
        depth_ = 0;
        generation_ = 0;
    }

    [[nodiscard]] auto active() const noexcept -> bool {
        return depth_ != 0;
    }

private:
    ResourceOperation operation_{ResourceOperation::crafting};
    std::uint64_t generation_{};
    std::uint8_t depth_{};
};

inline constexpr float kBuildUnionTimeoutSeconds = 0.75F;

class BuildUnionWindow {
public:
    [[nodiscard]] auto open(const std::uint64_t generation) noexcept -> bool {
        if (opened_) {
            return false;
        }
        opened_ = true;
        generation_ = generation;
        elapsed_ = 0.0F;
        return true;
    }

    [[nodiscard]] auto advance(const float deltaSeconds, const std::uint64_t generation) noexcept
        -> bool {
        if (!opened_) {
            return false;
        }
        if (generation != generation_) {
            reset();
            return true;
        }
        elapsed_ += std::max(deltaSeconds, 0.0F);
        if (elapsed_ >= kBuildUnionTimeoutSeconds) {
            reset();
            return true;
        }
        return false;
    }

    auto reset() noexcept -> void {
        opened_ = false;
        generation_ = 0;
        elapsed_ = 0.0F;
    }

    [[nodiscard]] auto opened() const noexcept -> bool {
        return opened_;
    }

private:
    bool opened_{};
    std::uint64_t generation_{};
    float elapsed_{};
};

struct BaseResourceSharingStatus {
    bool enabled{};
    bool worldAccessible{true};
    bool detectingCapabilities{};
    std::size_t baseCount{};
    std::size_t containerCount{};
    bool craftingAvailable{};
    bool buildingAvailable{};
    bool repairAvailable{};
    std::string craftingError;
    std::string buildingError;
    std::string repairError;
    std::string runtimeError;
};

[[nodiscard]] inline auto format_status(const BaseResourceSharingStatus& status) -> std::string {
    if (!status.enabled) {
        return "据点资源共享已关闭。";
    }
    if (!status.worldAccessible) {
        return "正在等待可访问的游戏世界。";
    }
    if (status.detectingCapabilities) {
        std::string text = "正在检测 Palworld 资源接口。";
        if (!status.craftingError.empty()) {
            text += "\n制作：" + status.craftingError;
        }
        if (!status.buildingError.empty()) {
            text += "\n建造：" + status.buildingError;
        }
        return text;
    }

    std::string text = "已发现 " + std::to_string(status.baseCount) + " 个据点、" +
                       std::to_string(status.containerCount) + " 个资源容器。\n";
    text += status.craftingAvailable ? "制作：可用" : "制作：不可用";
    text += status.buildingAvailable ? "；建造：可用" : "；建造：不可用";
    text += status.repairAvailable ? "；修理：可用" : "；修理：不可用";
    if (!status.craftingAvailable && !status.craftingError.empty()) {
        text += "\n制作：" + status.craftingError;
    }
    if (!status.buildingAvailable && !status.buildingError.empty()) {
        text += "\n建造：" + status.buildingError;
    }
    if (!status.repairAvailable && !status.repairError.empty()) {
        text += "\n" + status.repairError;
    }
    if (!status.runtimeError.empty()) {
        text += "\n错误：" + status.runtimeError;
    }
    return text;
}
}  // namespace base_resource_sharing
