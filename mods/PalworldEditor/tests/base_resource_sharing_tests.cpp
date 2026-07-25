#include <filesystem>
#include <iostream>
#include <system_error>

#include <base_resource_sharing/settings.hpp>

namespace {
auto failures = 0;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
}  // namespace

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_settings_default_off_and_round_trip() {
    using namespace base_resource_sharing;

    const auto missing = parse_settings("");
    CHECK(!missing.settings.enabled);
    CHECK(!missing.error.empty());

    const auto enabled = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=true\n");
    CHECK(enabled.settings.enabled);
    CHECK(enabled.error.empty());
    CHECK(serialize_settings(enabled.settings) ==
          "[BaseResourceSharing]\nEnabled=true\n");

    const auto invalid = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=maybe\n");
    CHECK(!invalid.settings.enabled);
    CHECK(!invalid.error.empty());
}

void test_settings_file_round_trip() {
    using namespace base_resource_sharing;

    const auto root = std::filesystem::temp_directory_path() /
                      "PalworldEditorBaseResourceSharingTests";
    const auto path = root / "config.ini";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    CHECK(save_settings(path, Settings{.enabled = true}).empty());
    const auto loaded = load_settings(path);
    CHECK(loaded.settings.enabled);
    CHECK(loaded.error.empty());

    std::filesystem::remove_all(root, ignored);
}

auto main() -> int {
    test_settings_default_off_and_round_trip();
    test_settings_file_round_trip();
    return failures == 0 ? 0 : 1;
}
