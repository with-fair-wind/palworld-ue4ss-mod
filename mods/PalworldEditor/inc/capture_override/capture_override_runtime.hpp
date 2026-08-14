/**
 * @file capture_override_runtime.hpp
 * @brief 声明按需注册投球 pre-hook、在回调中清除不可捕获标志的游戏线程运行时。
 * @details 仅在游戏线程调用；所有 Unreal 指针都是当次回调链内的非拥有句柄。
 */
#pragma once

#include <capture_override/capture_override_state.hpp>

#include <cstdint>
#include <vector>

namespace RC::Unreal {
class UFunction;
}

/** @brief 提供捕获不可捕获帕鲁功能的 Palworld 反射适配。 */
namespace capture_override {

/**
 * @brief 管理 4 个投球 pre-hook 的注册/注销，并在回调内实时写入目标帕鲁的捕获标志。
 * @details 开启时注册 hook，每次投球 pre-hook 清除目标帕鲁的不可捕获/Boss 标志；
 *          关闭或 LoadMap 前注销全部 hook。不保存原值、不恢复（单向写入）。
 *          forceHundredPercent 变化无需重注册 hook（回调内实时读取配置）。
 */
class CaptureOverrideRuntime final {
public:
    CaptureOverrideRuntime() = default;
    ~CaptureOverrideRuntime();
    CaptureOverrideRuntime(const CaptureOverrideRuntime&) = delete;
    auto operator=(const CaptureOverrideRuntime&) -> CaptureOverrideRuntime& = delete;
    CaptureOverrideRuntime(CaptureOverrideRuntime&&) noexcept = default;
    auto operator=(CaptureOverrideRuntime&&) noexcept -> CaptureOverrideRuntime& = default;

    /**
     * @brief 消费最新配置，按需注册或注销 hook。
     * @param[in] config 主开关与强制成功率子选项。
     * @note 必须在游戏线程调用。
     */
    auto set_config(const CaptureOverrideConfig& config) -> void;

    /** @brief 进入新世界时重置安全停用状态与阶段。 */
    auto on_world_begin(std::uint64_t generation) -> void;

    /** @brief 离开世界（LoadMap 前）时注销全部 hook。 */
    auto on_world_end() -> void;

    /** @brief 卸载前注销全部 hook；不得访问 Unreal。 */
    auto shutdown() -> void;

    /** @return 当前运行阶段。 */
    [[nodiscard]] auto phase() const noexcept -> CaptureRuntimePhase;

private:
    auto ensure_hooks_registered() -> void;
    auto unregister_hooks() -> void;

    CaptureOverrideConfig config_{};
    CaptureRuntimePhase phase_{CaptureRuntimePhase::off};
    bool hooksRegistered_{false};
    bool safetyDisabled_{false};
    /** @brief 已注册的 hook（function + pre-callback id），逆序注销。 */
    struct RegisteredHook {
        RC::Unreal::UFunction* function{};
        int32_t hookId{-1};
    };
    std::vector<RegisteredHook> hooks_;
};

}  // namespace capture_override
