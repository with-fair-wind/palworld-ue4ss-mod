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
 * @note 仅保存在进程内原子量中，不跨进程持久化。
 */
struct CaptureOverrideConfig {
    /** @brief 主开关：清除不可捕获/Boss 标志使捕获判定通过。 */
    bool enabled{false};
    /** @brief 子选项：额外强制接近 100% 捕获成功率与强制可捕获。 */
    bool forceHundredPercent{false};
};

/** @brief 游戏线程发布、GUI 只读的运行阶段。 */
enum class CaptureRuntimePhase : std::uint8_t {
    off,              /**< 未启用，hook 未注册。 */
    hooksRegistered,  /**< hook 已注册，等待投球时实时清除标志。 */
    safetyDisabled,   /**< hook 注册失败，本世界安全停用，切换开关不会绕过。 */
};

}  // namespace capture_override
