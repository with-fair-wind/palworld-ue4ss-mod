#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace base_resource_sharing {
struct Settings {
    bool enabled{};
};

struct SettingsParseResult {
    Settings settings;
    std::string error;
};

auto parse_settings(std::string_view text) -> SettingsParseResult;
auto serialize_settings(const Settings& settings) -> std::string;
auto load_settings(const std::filesystem::path& path) -> SettingsParseResult;
auto save_settings(const std::filesystem::path& path, const Settings& settings) -> std::string;
}  // namespace base_resource_sharing
