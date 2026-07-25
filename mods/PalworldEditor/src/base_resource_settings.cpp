#include <base_resource_sharing/settings.hpp>

#include <Windows.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <system_error>

namespace base_resource_sharing {
namespace {
auto trim_ascii(const std::string_view text) -> std::string_view {
    const auto first = text.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string_view::npos) {
        return {};
    }

    const auto last = text.find_last_not_of(" \t\n\r\f\v");
    return text.substr(first, last - first + 1);
}

auto equal_ascii_case_insensitive(const std::string_view left, const std::string_view right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index{}; index < left.size(); ++index) {
        const auto lower = [](const char character) {
            return character >= 'A' && character <= 'Z'
                       ? static_cast<char>(character - 'A' + 'a')
                       : character;
        };
        if (lower(left[index]) != lower(right[index])) {
            return false;
        }
    }
    return true;
}

auto parse_error(const std::string_view message) -> SettingsParseResult {
    return {.error = std::string{message}};
}

auto win32_error(const std::string_view action, const DWORD error) -> std::string {
    return std::string{action} + "，Win32 错误代码：" + std::to_string(error);
}
}  // namespace

auto parse_settings(const std::string_view text) -> SettingsParseResult {
    bool foundSection{};
    bool foundEnabled{};
    Settings settings;

    std::size_t lineStart{};
    while (lineStart < text.size()) {
        const auto lineEnd = text.find('\n', lineStart);
        const auto count = lineEnd == std::string_view::npos ? text.size() - lineStart
                                                              : lineEnd - lineStart;
        const auto line = trim_ascii(text.substr(lineStart, count));
        if (line.empty()) {
            return parse_error("配置包含空白行。");
        }

        if (!foundSection) {
            if (line != "[BaseResourceSharing]") {
                return parse_error("缺少 [BaseResourceSharing] 配置节。");
            }
            foundSection = true;
        } else {
            const auto equals = line.find('=');
            if (equals == std::string_view::npos || line.find('=', equals + 1) != std::string_view::npos) {
                return parse_error("配置行格式无效。");
            }

            const auto key = trim_ascii(line.substr(0, equals));
            const auto value = trim_ascii(line.substr(equals + 1));
            if (!equal_ascii_case_insensitive(key, "Enabled")) {
                return parse_error("配置包含未知键。");
            }
            if (foundEnabled) {
                return parse_error("Enabled 配置重复。");
            }
            if (equal_ascii_case_insensitive(value, "true")) {
                settings.enabled = true;
            } else if (!equal_ascii_case_insensitive(value, "false")) {
                return parse_error("Enabled 必须为 true 或 false。");
            }
            foundEnabled = true;
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }

    if (!foundSection) {
        return parse_error("缺少 [BaseResourceSharing] 配置节。");
    }
    if (!foundEnabled) {
        return parse_error("缺少 Enabled 配置。");
    }
    return {.settings = settings};
}

auto serialize_settings(const Settings& settings) -> std::string {
    return std::string{"[BaseResourceSharing]\nEnabled="} + (settings.enabled ? "true\n" : "false\n");
}

auto load_settings(const std::filesystem::path& path) -> SettingsParseResult {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) {
        return parse_error("无法检查配置文件：" + error.message());
    }
    if (!exists) {
        return parse_error("配置文件不存在，已使用默认关闭。");
    }

    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return parse_error("无法读取配置文件。");
    }
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        return parse_error("读取配置文件失败。");
    }
    return parse_settings(text);
}

auto save_settings(const std::filesystem::path& path, const Settings& settings) -> std::string {
    const auto parent = path.parent_path();
    std::error_code error;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return "无法创建配置目录：" + error.message();
        }
    }

    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            return "无法写入临时配置文件。";
        }
        output << serialize_settings(settings);
        output.flush();
        if (!output) {
            return "写入临时配置文件失败。";
        }
        output.close();
        if (!output) {
            return "关闭临时配置文件失败。";
        }
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto moveError = GetLastError();
        return win32_error("无法原子替换配置文件", moveError);
    }
    return {};
}
}  // namespace base_resource_sharing
