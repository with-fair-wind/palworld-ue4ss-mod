/**
 * @file settings.hpp
 * @brief 声明 PalworldEditor 各功能模块配置的聚合、解析与原子持久化接口。
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <base_resource_sharing/settings.hpp>
#include <grappling_hook/settings.hpp>

/** @brief 提供编辑器级配置聚合，不把独立功能耦合到彼此的命名空间。 */
namespace editor_settings {

/** @brief `config.ini` 中所有功能模块的纯值配置。 */
struct Settings {
    base_resource_sharing::Settings baseResourceSharing; /**< 跨据点资源共享配置。 */
    grappling_hook::Settings grapplingHook;              /**< 爪钩枪功能配置。 */
};

/** @brief 配置解析或加载结果；失败时 `settings` 保持默认全关。 */
struct SettingsParseResult {
    Settings settings; /**< 成功解析的配置，或失败安全的默认配置。 */
    std::string error; /**< 空表示成功，否则为面向用户的诊断。 */
};

/** @brief 从 UTF-8 INI 文本解析全部支持的配置节。 */
auto parse_settings(std::string_view text) -> SettingsParseResult;
/** @brief 把全部模块配置序列化为规范 UTF-8 INI 文本。 */
auto serialize_settings(const Settings& settings) -> std::string;
/** @brief 从指定路径加载配置；不存在或读取失败时返回默认全关及错误。 */
auto load_settings(const std::filesystem::path& path) -> SettingsParseResult;
/** @brief 通过同目录临时文件原子保存配置。 */
auto save_settings(const std::filesystem::path& path, const Settings& settings) -> std::string;

}  // namespace editor_settings
