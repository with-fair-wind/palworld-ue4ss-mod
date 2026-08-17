/**
 * @file capture_override_state.hpp
 * @brief 捕获不可捕获帕鲁功能的纯值配置与运行阶段。
 * @details 不包含任何 Unreal/UE4SS 头；GUI 线程写入配置镜像，游戏线程消费后决定是否
 *          注册/注销投球 pre-hook。运行阶段由游戏线程发布、GUI 只读。
 */
#pragma once

#include <cstdint>

/** @brief 解锁不可捕获帕鲁捕获的纯值领域类型。 */
namespace capture_override {

/**
 * @brief GUI 提交、游戏线程消费的配置镜像。
 * @note 仅保存在进程内原子量中，不跨进程持久化。两个开关语义正交：解锁改变
 *       捕获资格，强制改变捕获概率，可各自独立启用。
 */
struct CaptureOverrideConfig {
    /** @brief 解锁开关：临时清除不可捕获/Boss/NPC 门控，使目标进入捕获判定。 */
    bool unlockUncapturable{false};
    /** @brief 强制开关：临时强制接近 100% 捕获成功率与强制可捕获。 */
    bool forceHundredPercent{false};

    /** @retval true 任一开关启用，投球 Hook 应保持登记。 */
    [[nodiscard]] constexpr auto any_active() const noexcept -> bool {
        return unlockUncapturable || forceHundredPercent;
    }
};

/** @brief 游戏线程发布、GUI 只读的运行阶段。 */
enum class CaptureRuntimePhase : std::uint8_t {
    off,             /**< 未启用，hook 未注册。 */
    hooksRegistered, /**< hook 已注册，等待投球时实时清除标志。 */
    safetyDisabled,  /**< hook 注册或字段事务失败，本世界安全停用。 */
};

/** @brief 单次捕获目标事务预检结果。 */
enum class CapturePreparationStatus : std::uint8_t {
    ready,        /**< 目标完整，可建立临时覆盖事务。 */
    unavailable,  /**< 本次目标为空、正在消失或组件暂未就绪；仅跳过本次调用。 */
    incompatible, /**< 运行时字段或函数签名不兼容；本世界必须安全停用。 */
};

/** @brief 捕获覆盖功能唯一的纯值生命周期状态所有者。 */
class CaptureOverrideState final {
public:
    /** @brief 更新用户期望；安全停用状态不会被同世界内切换开关绕过。 */
    auto set_config(const CaptureOverrideConfig& config) noexcept -> void {
        config_ = config;
    }

    /** @brief 新世界可访问；保留进程内用户配置并解除上一世界的安全停用。 */
    auto begin_world() noexcept -> void {
        worldAccessible_ = true;
        phase_ = CaptureRuntimePhase::off;
    }

    /** @brief 世界即将销毁；保留用户配置供下一世界恢复。 */
    auto end_world() noexcept -> void {
        worldAccessible_ = false;
        phase_ = CaptureRuntimePhase::off;
    }

    /** @brief 标记全部必需 Hook 已完整登记。 */
    auto hooks_registered() noexcept -> void {
        phase_ = CaptureRuntimePhase::hooksRegistered;
    }

    /** @brief 标记 Hook 已完整注销；安全停用状态保持不变。 */
    auto hooks_removed() noexcept -> void {
        if (phase_ != CaptureRuntimePhase::safetyDisabled) {
            phase_ = CaptureRuntimePhase::off;
        }
    }

    /** @brief 本世界发生结构性或回滚失败，禁止重新启用。 */
    auto disable_for_world() noexcept -> void {
        phase_ = CaptureRuntimePhase::safetyDisabled;
    }

    /** @brief 消费单次目标预检结果；暂时不可用不改变功能状态。 */
    auto observe_preparation_status(const CapturePreparationStatus status) noexcept -> void {
        if (status == CapturePreparationStatus::incompatible) {
            disable_for_world();
        }
    }

    /** @retval true 当前应尝试登记全部 Hook。 */
    [[nodiscard]] auto should_register_hooks() const noexcept -> bool {
        return worldAccessible_ && config_.any_active() && phase_ == CaptureRuntimePhase::off;
    }

    /** @retval true 已登记 Hook 不再应保持。 */
    [[nodiscard]] auto should_remove_hooks() const noexcept -> bool {
        return phase_ == CaptureRuntimePhase::hooksRegistered &&
               (!worldAccessible_ || !config_.any_active());
    }

    /** @return 当前配置。 */
    [[nodiscard]] auto config() const noexcept -> CaptureOverrideConfig {
        return config_;
    }

    /** @return 当前运行阶段。 */
    [[nodiscard]] auto phase() const noexcept -> CaptureRuntimePhase {
        return phase_;
    }

private:
    CaptureOverrideConfig config_{};
    CaptureRuntimePhase phase_{CaptureRuntimePhase::off};
    bool worldAccessible_{};
};

}  // namespace capture_override
