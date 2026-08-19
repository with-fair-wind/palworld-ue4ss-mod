/**
 * @file waypoint_teleport_runtime.hpp
 * @brief 传送至最近地图标记点的游戏线程运行时：门控、标记读取与原生 SyncTeleport。
 * @details 只在游戏线程调用；跨帧不持有 UObject 指针。每帧开销固定为按键轮询；
 *          全部游戏逻辑仅在按键上升沿或 GUI 请求时一次性执行。结构故障 → 本世界
 *          代次内停用；LoadMap 后重新可用。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <common/hotkey_edge_trigger.hpp>
#include <skills/world_session_state.hpp>
#include <waypoint_teleport/waypoint_teleport_domain.hpp>

namespace waypoint_teleport {

/** @brief 提供给 GUI 线程的纯值快照。 */
struct WaypointTeleportSnapshot {
    WaypointTeleportConfig config; /**< 当前配置。 */
    bool domainDisabled{};         /**< 本世界代次是否已停用。 */
    std::string lastMessage;       /**< 最近一次触发或停用的说明。 */
    std::uint64_t teleportCount{}; /**< 成功传送次数。 */
    std::uint64_t failCount{};     /**< 失败/拦截累计次数。 */
};

/** @brief 标记点传送运行时；仅游戏线程调用（snapshot/set_config/request_teleport 除外）。 */
class WaypointTeleportRuntime {
public:
    /**
     * @brief 从 ini 文件加载配置；文件缺失/损坏回退默认值。
     * @param[in] iniPath 配置文件完整路径；保存用于 set_config 写回。
     */
    auto load_config(std::string_view iniPath) -> void;

    /**
     * @brief 更新配置并写回 ini（GUI 线程调用）。
     * @note 配置变化由下一帧 tick 在游戏线程重置按键状态机。
     */
    auto set_config(WaypointTeleportConfig config) -> void;

    /**
     * @brief 每帧推进：按键轮询 + GUI 请求消费；上升沿时执行完整管线。
     * @param[in] deltaSeconds 帧间隔（保留签名以匹配调用约定）。
     * @param[in] session 世界会话状态；仅在世界可访问时执行。
     */
    auto tick(float deltaSeconds, const skill_editor::WorldSessionState& session) -> void;

    /** @brief GUI 测试按钮：请求一次传送，下一帧 tick 消费（线程安全）。 */
    auto request_teleport() -> void;

    /** @brief LoadMap 前调用：重置触发状态与域停用。 */
    auto begin_world_transition() -> void;

    /** @brief LoadMap 完成后调用：无操作（无跨世界缓存）。 */
    auto finish_world_transition() -> void;

    /** @brief 读取供 GUI 显示的纯值快照（线程安全）。 */
    [[nodiscard]] auto snapshot() const -> WaypointTeleportSnapshot;

private:
    /** @brief 远距目标的第二段贴地计划（等待目标区块随玩家流送加载）。 */
    struct PendingRefinement {
        bool active{};
        double x{};
        double y{};
        double anchorZ{};
        std::chrono::steady_clock::time_point deadline{};
    };

    /** @brief 在 tick 内执行一次完整触发管线并返回结果。 */
    auto execute_trigger(const WaypointTeleportConfig& config) -> WaypointTeleportResult;

    /** @brief 到期执行远距目标的贴地校正（best-effort，失败仅取消计划）。 */
    auto run_pending_refinement() -> void;

    /** @brief 门控 + 地面追踪 + 放置 + 删除的共用传送核心（F7 与地图点击共用）。 */
    auto teleport_to_candidate(const WaypointTeleportConfig& config, RC::Unreal::UObject* manager,
                               const MarkerCandidate& target) -> WaypointTeleportResult;

    auto set_disabled(const std::string& message) -> void;

    /** @brief 记录界面最近消息；isFailure 为真时累计失败计数。 */
    auto note(const std::string& message, bool isFailure) -> void;

    WaypointTeleportConfig config_{kDefaultWaypointTeleportConfig};
    pal_game::HotkeyEdgeTrigger trigger_;
    PendingRefinement pendingRefinement_;
    std::string iniPath_;
    std::atomic<bool> requestedTeleport_{false};
    /** @brief GUI 写入配置后置位；下一帧 tick 在游戏线程重置按键状态机。 */
    std::atomic<bool> configDirty_{false};
    std::atomic<bool> domainDisabled_{false};
    std::string lastMessage_;
    std::uint64_t teleportCount_{};
    std::uint64_t failCount_{};
    mutable std::mutex snapshotMutex_;
};

}  // namespace waypoint_teleport
