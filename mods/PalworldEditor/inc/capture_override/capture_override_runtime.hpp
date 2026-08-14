/**
 * @file capture_override_runtime.hpp
 * @brief 声明可逆捕获覆盖事务与按需 UFunction Hook 运行时。
 */
#pragma once

#include <memory>

#include <capture_override/capture_override_state.hpp>

namespace capture_override {

/**
 * @brief 管理投球 Hook，并仅在单次 UFunction 调用期间临时覆盖捕获字段。
 * @details 全部 Hook 先完成精确签名预检，再以全有或全无方式登记；字段在 pre-hook
 *          快照并写入，在配对 post-hook 恢复。关闭、LoadMap 和卸载均对称注销 Hook。
 */
class CaptureOverrideRuntime final {
public:
    CaptureOverrideRuntime();
    ~CaptureOverrideRuntime();
    CaptureOverrideRuntime(const CaptureOverrideRuntime&) = delete;
    auto operator=(const CaptureOverrideRuntime&) -> CaptureOverrideRuntime& = delete;
    CaptureOverrideRuntime(CaptureOverrideRuntime&&) = delete;
    auto operator=(CaptureOverrideRuntime&&) -> CaptureOverrideRuntime& = delete;

    /** @brief 消费进程内用户配置，并按需登记或注销 Hook。 */
    auto set_config(const CaptureOverrideConfig& config) -> void;

    /** @brief 新世界可访问时恢复进程内用户期望。 */
    auto on_world_begin() -> void;

    /** @brief LoadMap 前注销 Hook 并撤销写权限，但保留进程内配置。 */
    auto on_world_end() -> void;

    /** @brief EngineTick 常量时间维护；处理回调请求的延迟安全注销。 */
    auto tick() -> void;

    /** @brief 卸载前注销全部 Hook。 */
    auto shutdown() -> void;

    /** @return 当前运行阶段。 */
    [[nodiscard]] auto phase() const noexcept -> CaptureRuntimePhase;

private:
    struct Impl;

    auto reconcile_hooks() -> void;
    auto ensure_hooks_registered() -> void;
    auto unregister_hooks() -> void;

    CaptureOverrideState state_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace capture_override
