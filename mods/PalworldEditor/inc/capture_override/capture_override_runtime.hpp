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

    /**
     * @brief 卸载前恢复在途事务并注销全部 Hook。
     * @retval true 全部在途事务均已恢复。
     * @retval false 至少一个事务恢复失败；调用方必须保留实例并放弃热卸载。
     * @note 失败是永久性的：失败的 pending 事务已被丢弃，锁存后任何重试都无法挽回，
     *       调用方应把 false 映射为卸载清理结果的 permanentFailure。
     * @warning 只允许在游戏线程调用；仅返回 true 后才可析构对象。
     */
    [[nodiscard]] auto shutdown() -> bool;

    /** @return 当前运行阶段。 */
    [[nodiscard]] auto phase() const noexcept -> CaptureRuntimePhase;

private:
    struct Impl;

    auto reconcile_hooks() -> void;
    auto ensure_hooks_registered() -> void;
    /**
     * @retval true 全部在途事务均已恢复。
     * @retval false 至少一个事务恢复失败；失败状态上报调用方，Unreal 句柄不会跨帧保留。
     * @note unregister_hooks 的运行期路径（关闭开关/LoadMap/安全停用）会丢弃该结果——
     *       域级安全停用已在 restore 内生效；仅 shutdown 需要消费结果锁存失败。
     */
    [[nodiscard]] auto restore_pending_transactions() -> bool;
    auto unregister_hooks() -> void;

    CaptureOverrideState state_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace capture_override
