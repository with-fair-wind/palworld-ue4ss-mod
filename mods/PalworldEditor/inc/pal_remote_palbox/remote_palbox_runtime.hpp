/**
 * @file remote_palbox_runtime.hpp
 * @brief 远程终端的游戏线程运行时：门控、基地解析、原生 HUD Push。
 * @details 只在游戏线程调用；跨帧不持有 UObject 指针。每帧开销固定为 2 次 WinAPI
 *          调用；全部游戏逻辑仅在按键上升沿或 GUI 请求时一次性执行。
 *          结构故障 → 本世界代次内停用；LoadMap 后重新探测。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <pal_remote_palbox/remote_palbox.hpp>
#include <pal_remote_palbox/remote_palbox_config.hpp>
#include <skills/world_session_state.hpp>

namespace pal_remote_palbox {

/** @brief 一次触发的结果分类，供 UI 与日志使用。 */
enum class RemotePalboxTriggerResult : std::uint8_t {
    opened,      /**< Push 成功且返回了有效 widget ID。 */
    blocked,     /**< 被门控拦截（地牢/骑乘/圈外/战斗）。 */
    noBase,      /**< 没有可用的已拥有基地。 */
    unavailable, /**< 反射链路不可用或 Push 未返回有效 ID。 */
    disabled,    /**< 域已停用（结构故障或连续超时）。 */
};

/** @brief 提供给 GUI 线程的纯值快照。 */
struct RemotePalboxSnapshot {
    RemotePalboxConfig config; /**< 当前配置。 */
    bool domainDisabled{};     /**< 本世界代次是否已停用。 */
    std::string lastMessage;   /**< 最近一次触发或停用的说明。 */
    std::uint64_t openCount{}; /**< 成功打开次数。 */
    std::uint64_t failCount{}; /**< 失败/拦截累计次数。 */
};

/** @brief 远程终端运行时；仅游戏线程调用（snapshot/set_config/request_open 除外）。 */
class RemotePalboxRuntime {
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
    auto set_config(RemotePalboxConfig config) -> void;

    /**
     * @brief 每帧推进：WinAPI 按键轮询 + GUI 请求消费；上升沿时执行完整管线。
     * @param[in] deltaSeconds 帧间隔（本版本未使用，保留签名以匹配调用约定）。
     * @param[in] session 世界会话状态；仅在世界可访问时执行。
     */
    auto tick(float deltaSeconds, const skill_editor::WorldSessionState& session) -> void;

    /** @brief GUI 测试按钮：请求一次打开，下一帧 tick 消费（线程安全）。 */
    auto request_open() -> void;

    /** @brief LoadMap 前调用：重置触发状态、域停用与请求标志。 */
    auto begin_world_transition() -> void;

    /** @brief LoadMap 完成后调用：无操作（widget 路径字符串缓存跨世界保留）。 */
    auto finish_world_transition() -> void;

    /** @brief 读取供 GUI 显示的纯值快照（线程安全）。 */
    [[nodiscard]] auto snapshot() const -> RemotePalboxSnapshot;

private:
    /** @brief 在 tick 内执行一次完整触发管线并返回结果；config 为 tick 内加锁拷贝的快照。 */
    auto execute_trigger(const RemotePalboxConfig& config) -> RemotePalboxTriggerResult;

    /**
     * @brief 探测关键反射点；服务对象尚未创建时允许重试，已创建对象缺少契约函数时停用域。
     */
    auto probe_domain() -> bool;

    auto set_disabled(const std::string& message) -> void;

    /** @brief 记录界面最近消息；isFailure 为真时累计失败计数（仅真实触发失败计）。 */
    auto note(const std::string& message, bool isFailure) -> void;

    RemotePalboxConfig config_{kDefaultRemotePalboxConfig};
    HotkeyEdgeTrigger trigger_;
    std::string iniPath_;
    std::atomic<bool> requestedOpen_{false};
    /** @brief GUI 写入配置后置位；下一帧 tick 在游戏线程重置按键状态机。 */
    std::atomic<bool> configDirty_{false};
    std::atomic<bool> domainDisabled_{false};
    bool domainProbed_{};
    std::string widgetPath_; /**< 终端界面类的完整路径；跨世界保留。 */
    std::string lastMessage_;
    std::uint64_t openCount_{};
    std::uint64_t failCount_{};
    std::uint64_t consecutiveTimeoutCount_{};
    mutable std::mutex snapshotMutex_;
};
}  // namespace pal_remote_palbox
