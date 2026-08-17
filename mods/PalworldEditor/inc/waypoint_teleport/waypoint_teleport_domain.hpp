/**
 * @file waypoint_teleport_domain.hpp
 * @brief 传送至地图标记点的纯值层：ini 配置解析与最近标记选择策略。
 * @details 只依赖标准库，不接触 Unreal；游戏线程适配见 waypoint_teleport_runtime。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <common/ini_config.hpp>

/** @brief 提供传送至地图标记点的纯值领域逻辑。 */
namespace waypoint_teleport {

/** @brief 自定义地图标记的防御性数量上限。 */
inline constexpr std::size_t kMaximumCustomMarkers{1024};

/**
 * @brief 运行时配置；与 waypoint_teleport.ini 的键一一对应。
 * @note 快捷键默认 F7（0x76=118），避开 UE4SS GUI 的 F10。
 */
struct WaypointTeleportConfig {
    int hotkeyVk{118};               /**< 触发快捷键 VK 码。 */
    bool disableWhileMounted{true};  /**< 骑乘时禁用。 */
    bool disableInDungeon{true};     /**< 地牢内禁用。 */
    bool disableDuringCombat{true};  /**< 战斗中禁用。 */
    float arrivalHeightOffset{10000.0F}; /**< 到达高度偏移（厘米）；0 = 使用标记原始 Z。 */
};

inline constexpr WaypointTeleportConfig kDefaultWaypointTeleportConfig{};

/** @brief 一次触发的结果分类，供 UI 与日志使用。 */
enum class WaypointTeleportResult : std::uint8_t {
    teleported,  /**< 已调用原生 SyncTeleport。 */
    blocked,     /**< 被门控拦截（地牢/骑乘/战斗/世界未同步）。 */
    noMarker,    /**< 当前没有自定义地图标记。 */
    unavailable, /**< 反射链路不可用（控制器/管理器/组件/函数）。 */
    disabled,    /**< 域已因结构故障停用。 */
};

/** @brief 标记候选：运行时读取后交给纯值选择器。 */
struct MarkerCandidate {
    double x{};                /**< 标记世界坐标 X。 */
    double y{};                /**< 标记世界坐标 Y。 */
    double z{};                /**< 标记世界坐标 Z（到达点锚定高度）。 */
    double distanceSquared{};  /**< 玩家到标记的水平距离平方。 */
};

/**
 * @brief 选择传送目标：水平距离最近的标记。
 * @param[in] candidates 运行时按域上限读取的标记候选。
 * @return 最近候选的下标；无候选时为空。
 */
[[nodiscard]] inline auto select_nearest_marker(
    const std::span<const MarkerCandidate> candidates) -> std::optional<std::size_t> {
    std::optional<std::size_t> nearest;
    for (std::size_t index{}; index < candidates.size(); ++index) {
        if (!nearest.has_value() ||
            candidates[index].distanceSquared < candidates[*nearest].distanceSquared) {
            nearest = index;
        }
    }
    return nearest;
}

/** @brief 键位合法性使用公共原语 pal_game::valid_hotkey_vk。 */

/** @brief 到达偏移必须是有界厘米值（±1e6 = ±10km），拒绝 NaN/Inf。 */
[[nodiscard]] inline auto valid_arrival_offset(const float offset) noexcept -> bool {
    return offset > -1000000.0F && offset < 1000000.0F;
}

/**
 * @brief 按行解析 `Key=Value` 配置。
 * @details 未知键忽略；非法值回退默认。
 */
[[nodiscard]] inline auto parse_waypoint_teleport_config(const std::string_view content)
    -> WaypointTeleportConfig {
    WaypointTeleportConfig config = kDefaultWaypointTeleportConfig;
    std::size_t pos{};
    while (pos < content.size()) {
        const auto eol = content.find('\n', pos);
        const auto line =
            content.substr(pos, eol == std::string_view::npos ? content.size() - pos : eol - pos);
        pos = eol == std::string_view::npos ? content.size() : eol + 1;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            continue;
        }
        const auto key = line.substr(0, eq);
        const auto value = line.substr(eq + 1);
        if (key == "HotkeyVk") {
            const auto parsed = pal_game::parse_ini_int(value);
            if (parsed.has_value() && pal_game::valid_hotkey_vk(*parsed)) {
                config.hotkeyVk = *parsed;
            }
        } else if (key == "DisableWhileMounted") {
            config.disableWhileMounted = pal_game::parse_ini_bool(value, config.disableWhileMounted);
        } else if (key == "DisableInDungeon") {
            config.disableInDungeon = pal_game::parse_ini_bool(value, config.disableInDungeon);
        } else if (key == "DisableDuringCombat") {
            config.disableDuringCombat = pal_game::parse_ini_bool(value, config.disableDuringCombat);
        } else if (key == "ArrivalHeightOffset") {
            const auto parsed = pal_game::parse_ini_float(value);
            if (parsed.has_value() && valid_arrival_offset(*parsed)) {
                config.arrivalHeightOffset = *parsed;
            }
        }
        // 未知键忽略。
    }
    return config;
}

/** @brief 固定键序序列化，与 parse 互逆。 */
[[nodiscard]] inline auto serialize_waypoint_teleport_config(const WaypointTeleportConfig& config)
    -> std::string {
    return "HotkeyVk=" + std::to_string(config.hotkeyVk) +
           "\n"
           "DisableWhileMounted=" +
           (config.disableWhileMounted ? "true" : "false") +
           "\n"
           "DisableInDungeon=" +
           (config.disableInDungeon ? "true" : "false") +
           "\n"
           "DisableDuringCombat=" +
           (config.disableDuringCombat ? "true" : "false") +
           "\n"
           "ArrivalHeightOffset=" +
           std::to_string(config.arrivalHeightOffset) + "\n";
}

}  // namespace waypoint_teleport
