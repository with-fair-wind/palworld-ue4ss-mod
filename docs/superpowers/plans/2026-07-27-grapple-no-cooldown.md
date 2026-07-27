# 爪钩枪无冷却 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 UE4SS GUI 增加「爪钩枪无冷却」开关，开启时翻转游戏自带 `UPalDebugSetting.bDisableGrapplingCoolDown`；偏好持久化并在世界就绪时重应用。

**Architecture:** 沿用「据点资源共享」开关的生命周期——GUI 勾选设原子偏好 + dirty，`game_thread_tick` 见 dirty 在游戏线程应用，LoadMap/世界就绪后重应用。新增反射网关 `pal_game::set_grapple_no_cooldown(bool)`（`FindFirstOf("PalDebugSetting")` + `FBoolProperty` 写）。配置复用既有 settings 模块，扩展为 `[BaseResourceSharing]` + `[GrapplingHook]` 双节。

**Tech Stack:** C++23、UE4SS 反射、ImGui、CMake/Ninja、现有 `PalworldEditorTests` 与 `PalworldEditorBaseResourceSharingTests`。

## Global Constraints

- `bDisableGrapplingCoolDown` 是**全局**调试开关（影响整个游戏，不止当前存档）——符合本功能定位。
- 所有反射只在 `game_thread_tick` 游戏线程按需执行（仅在开关切换或世界就绪重应用时）；空闲帧零开销；无逐帧任务、无后台线程、无 `FindAllOf`（用 `FindFirstOf`）。
- 不缓存跨帧 `UObject*`/`FProperty*`；每次应用重新 `FindFirstOf` + 校验。
- 配置文件仍是单个 `config.ini`，含两个独立小节；缺失或无效时安全回退为关闭；保持「每节内严格、未知键/节拒绝」的失败安全语义。
- 不影响其他武器、帕鲁技能、伙伴技能冷却；不提供可调 CD 数值。
- 版本号在合并时确定（见 Task 4）。

---

## File Structure

- **Modify:** `mods/PalworldEditor/inc/base_resource_sharing/settings.hpp` — `Settings` 增加 `grappleNoCooldown`。
- **Modify:** `mods/PalworldEditor/src/base_resource_settings.cpp` — `parse_settings`/`serialize_settings` 双节化。
- **Modify:** `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp` — 更新既有 settings 测试 + 新增 grapple 节测试。
- **Modify:** `mods/PalworldEditor/inc/game/pal_game.hpp` — 新增 `set_grapple_no_cooldown(bool)`。
- **Modify:** `mods/PalworldEditor/src/dllmain.cpp` — 原子量、`on_program_start` 读取、`game_thread_tick` 应用、`finish_world_transition` 重应用、grapple checkbox GUI、更新资源共享 checkbox 的 save 以保留 grapple 字段。
- **Modify:** `README.md`、`AGENTS.md`、`CLAUDE.md` — 版本与文档。

---

## Task 1: 配置扩展为双节（Settings + 解析器 + 测试）

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/settings.hpp`
- Modify: `mods/PalworldEditor/src/base_resource_settings.cpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces: `base_resource_sharing::Settings` 新增 `bool grappleNoCooldown{};`；`parse_settings` 接受两节（均可选，顺序任意，节内严格）；`serialize_settings` 输出两节。Task 3 依赖 `loaded.settings.grappleNoCooldown` 与 `Settings{.enabled=..., .grappleNoCooldown=...}`。

- [ ] **Step 1: 先改/加失败测试**

替换 `test_settings_default_off_and_round_trip` 与 `test_settings_file_round_trip`，并新增 grapple 节测试。把现有两个 settings 测试函数整体替换为：

```cpp
void test_settings_default_off_and_round_trip() {
    using namespace base_resource_sharing;

    // 空配置：两节均缺省，不再视为错误（失败安全回退为全关）。
    const auto empty = parse_settings("");
    CHECK(!empty.settings.enabled);
    CHECK(!empty.settings.grappleNoCooldown);
    CHECK(empty.error.empty());

    // 仅资源共享节。
    const auto sharing = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=true\n");
    CHECK(sharing.settings.enabled);
    CHECK(!sharing.settings.grappleNoCooldown);
    CHECK(sharing.error.empty());

    // 仅爪钩枪节。
    const auto grapple = parse_settings(
        "[GrapplingHook]\n"
        "NoCooldown=true\n");
    CHECK(!grapple.settings.enabled);
    CHECK(grapple.settings.grappleNoCooldown);
    CHECK(grapple.error.empty());

    // 两节共存、任意顺序。
    const auto both = parse_settings(
        "[GrapplingHook]\n"
        "NoCooldown=true\n"
        "[BaseResourceSharing]\n"
        "Enabled=true\n");
    CHECK(both.settings.enabled);
    CHECK(both.settings.grappleNoCooldown);
    CHECK(both.error.empty());
    CHECK(serialize_settings(both.settings) ==
          "[BaseResourceSharing]\nEnabled=true\n[GrapplingHook]\nNoCooldown=true\n");

    // 节内非法值仍失败安全。
    const auto invalid = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=maybe\n");
    CHECK(!invalid.settings.enabled);
    CHECK(!invalid.error.empty());

    // 未知键仍拒绝。
    const auto unknown = parse_settings(
        "[GrapplingHook]\n"
        "Bogus=true\n");
    CHECK(!unknown.settings.grappleNoCooldown);
    CHECK(!unknown.error.empty());
}

void test_settings_file_round_trip() {
    using namespace base_resource_sharing;

    const auto root =
        std::filesystem::temp_directory_path() / "PalworldEditorBaseResourceSharingTests";
    const auto path = root / "config.ini";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const auto missing = load_settings(path);
    CHECK(!missing.settings.enabled);
    CHECK(!missing.settings.grappleNoCooldown);
    CHECK(!missing.error.empty());
    CHECK(!std::filesystem::exists(path));

    CHECK(save_settings(path, Settings{.enabled = true, .grappleNoCooldown = true}).empty());
    const auto loaded = load_settings(path);
    CHECK(loaded.settings.enabled);
    CHECK(loaded.settings.grappleNoCooldown);
    CHECK(loaded.error.empty());

    std::filesystem::remove_all(root, ignored);
}
```

`main()` 中调用名不变（`test_settings_default_off_and_round_trip();`、`test_settings_file_round_trip();`），无需改 `main()`。

- [ ] **Step 2: 运行测试并确认失败**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: 编译失败（`Settings` 无 `grappleNoCooldown`）或断言失败。

- [ ] **Step 3: 扩展 Settings 与序列化**

`settings.hpp` 的 `Settings` 改为：

```cpp
struct Settings {
    bool enabled{};
    bool grappleNoCooldown{};
};
```

`base_resource_settings.cpp` 的 `serialize_settings` 改为（顺序固定：资源共享在前，爪钩枪在后）：

```cpp
auto serialize_settings(const Settings& settings) -> std::string {
    return std::string{"[BaseResourceSharing]\nEnabled="} +
           (settings.enabled ? "true\n" : "false\n") +
           "[GrapplingHook]\nNoCooldown=" + (settings.grappleNoCooldown ? "true\n" : "false\n");
}
```

- [ ] **Step 4: 重写 parse_settings 为双节、节内严格**

把 `base_resource_settings.cpp` 的 `parse_settings` 整体替换为：

```cpp
auto parse_settings(const std::string_view text) -> SettingsParseResult {
    Settings settings;
    enum class Section { none, sharing, grapple };
    Section current{Section::none};
    bool seenSharing{};
    bool seenGrapple{};
    bool seenEnabled{};
    bool seenNoCooldown{};

    std::size_t lineStart{};
    while (lineStart < text.size()) {
        const auto lineEnd = text.find('\n', lineStart);
        const auto count =
            lineEnd == std::string_view::npos ? text.size() - lineStart : lineEnd - lineStart;
        const auto line = trim_ascii(text.substr(lineStart, count));
        if (line.empty()) {
            return parse_error("配置包含空白行。");
        }

        if (line == "[BaseResourceSharing]") {
            if (seenSharing) {
                return parse_error("[BaseResourceSharing] 配置节重复。");
            }
            seenSharing = true;
            current = Section::sharing;
            seenEnabled = false;
        } else if (line == "[GrapplingHook]") {
            if (seenGrapple) {
                return parse_error("[GrapplingHook] 配置节重复。");
            }
            seenGrapple = true;
            current = Section::grapple;
            seenNoCooldown = false;
        } else if (line.find('=') != std::string_view::npos) {
            const auto equals = line.find('=');
            if (line.find('=', equals + 1) != std::string_view::npos) {
                return parse_error("配置行格式无效。");
            }
            const auto key = trim_ascii(line.substr(0, equals));
            const auto value = trim_ascii(line.substr(equals + 1));

            if (current == Section::none) {
                return parse_error("配置键出现在任何节之前。");
            }
            if (current == Section::sharing && equal_ascii_case_insensitive(key, "Enabled")) {
                if (seenEnabled) {
                    return parse_error("Enabled 配置重复。");
                }
                if (equal_ascii_case_insensitive(value, "true")) {
                    settings.enabled = true;
                } else if (!equal_ascii_case_insensitive(value, "false")) {
                    return parse_error("Enabled 必须为 true 或 false。");
                }
                seenEnabled = true;
            } else if (current == Section::grapple &&
                       equal_ascii_case_insensitive(key, "NoCooldown")) {
                if (seenNoCooldown) {
                    return parse_error("NoCooldown 配置重复。");
                }
                if (equal_ascii_case_insensitive(value, "true")) {
                    settings.grappleNoCooldown = true;
                } else if (!equal_ascii_case_insensitive(value, "false")) {
                    return parse_error("NoCooldown 必须为 true 或 false。");
                }
                seenNoCooldown = true;
            } else {
                return parse_error("配置包含未知键。");
            }
        } else {
            return parse_error("配置行格式无效。");
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }
    return {.settings = settings};
}
```

> 说明：用 `current` 枚举跟踪当前所在节，两节顺序任意、均可选；空文本（`while (lineStart < text.size())` 不进入循环）→ 两个默认 false、无错误（失败安全）。未知节名（如 `[Bogus]`）会被末尾 `else` 分支判为「配置行格式无效」而拒绝。

- [ ] **Step 5: 运行测试并确认通过**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: 全部测试通过（两个测试可执行文件）。

- [ ] **Step 6: 提交配置扩展**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/settings.hpp mods/PalworldEditor/src/base_resource_settings.cpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: extend mod config with grappling hook section"
```

---

## Task 2: 爪钩枪无冷却反射网关

**Files:**
- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp`

**Interfaces:**
- Produces: `pal_game::set_grapple_no_cooldown(bool)`（游戏线程；`FindFirstOf("PalDebugSetting")` + 写 `bDisableGrapplingCoolDown`）。Task 3 在 `game_thread_tick` 调用。

- [ ] **Step 1: 新增网关函数**

在 `pal_game.hpp` 末尾、`discover_objects` 之前（或任意 `pal_game` 命名空间内）新增：

```cpp
/**
 * @brief 翻转游戏自带「爪钩枪无冷却」调试开关。
 * @param[in] enabled `true` 关闭爪钩枪冷却；`false` 恢复默认。
 * @warning 只能在游戏线程调用。`PalDebugSetting` 或属性不可用时静默返回（下个 dirty 周期重试）。
 */
inline auto set_grapple_no_cooldown(const bool enabled) -> void {
    auto* const debug = UObjectGlobals::FindFirstOf(STR("PalDebugSetting"));
    if (!is_valid(debug)) {
        return;
    }
    auto* const property = debug->GetPropertyByNameInChain(STR("bDisableGrapplingCoolDown"));
    auto* const boolProperty = CastField<FBoolProperty>(property);
    if (boolProperty == nullptr) {
        return;
    }
    boolProperty->SetPropertyValueInContainer(debug, enabled);
}
```

- [ ] **Step 2: 构建 DLL 验证反射类型**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected: DLL 编译链接成功；无 `FBoolProperty`/`GetPropertyByNameInChain`/`SetPropertyValueInContainer` API 错误。

- [ ] **Step 3: 提交网关**

```powershell
git add mods/PalworldEditor/inc/game/pal_game.hpp
git commit -m "feat: toggle grappling hook cooldown via PalDebugSetting"
```

---

## Task 3: dllmain 接入开关生命周期与 GUI

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

**Interfaces:**
- Consumes: `pal_game::set_grapple_no_cooldown`（Task 2）、`base_resource_sharing::Settings.grappleNoCooldown`（Task 1）、既有 `configPath_`/`requestedBaseSharingEnabled_`/`baseSharingSettingDirty_`。
- Produces: `requestedGrappleNoCooldown_`/`grappleSettingDirty_` 原子量、`grappleConfigError_`、`render_grapple_no_cooldown(self)`；资源共享 checkbox 的 save 改为保留 grapple 字段。

- [ ] **Step 1: 新增成员**

在既有 `requestedBaseSharingEnabled_`/`baseSharingSettingDirty_`（约 1112–1114 行）之后新增：

```cpp
    /** @brief 用户期望的爪钩枪无冷却偏好；EngineTick 应用到 PalDebugSetting。 */
    std::atomic<bool> requestedGrappleNoCooldown_{false};
    /** @brief 通知 EngineTick 把爪钩枪偏好应用到游戏（切换与世界就绪重应用）。 */
    std::atomic<bool> grappleSettingDirty_{false};
    /** @brief 爪钩枪配置持久化的最近错误（仅 GUI 显示）。 */
    std::string grappleConfigError_;
```

- [ ] **Step 2: on_program_start 读取 grapple 偏好**

在 `on_program_start()` 既有资源共享读取（`requestedBaseSharingEnabled_.store(loaded.settings.enabled);` 之后）补：

```cpp
        requestedGrappleNoCooldown_.store(loaded.settings.grappleNoCooldown);
        grappleSettingDirty_.store(true);
```

- [ ] **Step 3: game_thread_tick 应用**

在 `game_thread_tick` 既有资源共享应用块（`if (baseSharingSettingDirty_.exchange(false)) { baseResourceBridge_.set_enabled(...); }` 之后）补：

```cpp
        if (grappleSettingDirty_.exchange(false)) {
            pal_game::set_grapple_no_cooldown(requestedGrappleNoCooldown_.load());
        }
```

- [ ] **Step 4: 世界就绪后重应用**

在 `finish_world_transition()` 既有 `baseResourceBridge_.on_world_ready(...);` 之后补（让下一个 EngineTick 重新写一次 flag）：

```cpp
        grappleSettingDirty_.store(true);
```

- [ ] **Step 5: 新增 GUI 与主窗口挂接**

新增静态渲染函数（放在 `render_base_resource_sharing` 之后）：

```cpp
    /** @brief 渲染爪钩枪无冷却开关；切换时立即应用并持久化。 */
    static void render_grapple_no_cooldown(PalworldEditorMod* self) {
        bool enabled = self->requestedGrappleNoCooldown_.load();
        if (ImGui::Checkbox("爪钩枪无冷却", &enabled)) {
            self->requestedGrappleNoCooldown_.store(enabled);
            self->grappleSettingDirty_.store(true);
            self->grappleConfigError_ =
                self->configPath_.empty()
                    ? std::string{"配置路径尚未初始化，设置未持久化。"}
                    : base_resource_sharing::save_settings(
                          self->configPath_,
                          base_resource_sharing::Settings{
                              .enabled = self->requestedBaseSharingEnabled_.load(),
                              .grappleNoCooldown = enabled});
        }
        if (!self->grappleConfigError_.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F), "%s",
                               self->grappleConfigError_.c_str());
        }
        ImGui::TextDisabled("全局生效；开启后爪钩枪射击无冷却。");
    }
```

在主窗口渲染区既有 `render_base_resource_sharing(self);`（含其前后的 `ImGui::Separator()`）之后补：

```cpp
                ImGui::Separator();
                render_grapple_no_cooldown(self);
```

- [ ] **Step 6: 资源共享 checkbox 的 save 保留 grapple 字段**

把 `render_base_resource_sharing` 中既有 save 调用：

```cpp
                    : base_resource_sharing::save_settings(
                          self->configPath_, base_resource_sharing::Settings{.enabled = enabled});
```

改为（保留当前 grapple 偏好，避免持久化时清零）：

```cpp
                    : base_resource_sharing::save_settings(
                          self->configPath_,
                          base_resource_sharing::Settings{
                              .enabled = enabled,
                              .grappleNoCooldown = self->requestedGrappleNoCooldown_.load()});
```

- [ ] **Step 7: 格式检查、构建与测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
```

Expected: 格式检查、三个构建目标与全部测试通过。

- [ ] **Step 8: 提交接入**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp
git commit -m "feat: wire grappling hook no-cooldown toggle lifecycle"
```

---

## Task 4: 版本与文档

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: 升版本**

把 `dllmain.cpp` 三处版本字符串（`ModVersion`、加载日志、GUI 标题）从 `1.6.4` 改为目标号。**版本号说明：** 本仓库现有并行分支被动分类（1.6.5）与属性编辑器（1.6.6）都会改同样的三处字符串；本计划暂定 **1.6.7**（假设另两者先合并）。若本分支先合并，改为当时的下一个号；合并时这三处必然冲突，取最高号解决即可。

```powershell
rg -n "1\.6\.[4-7]" mods/PalworldEditor/src/dllmain.cpp
```

- [ ] **Step 2: 更新文档**

- README：「功能」表新增「爪钩枪无冷却」行；使用方法补一句（勾选即时无冷却，全局生效，持久化到 config.ini）；已知限制追加版本行。
- AGENTS.md / CLAUDE.md：架构层新增「爪钩枪无冷却：翻转 `PalDebugSetting.bDisableGrapplingCoolDown`，按需游戏线程应用、世界就绪重应用」；配置说明补 `[GrapplingHook] NoCooldown`；版本历史追加该版本行。

- [ ] **Step 3: 完整仓库验证**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
git status --short
```

Expected: 三目标成功；CTest 全通过；`git diff --check` 无输出。

- [ ] **Step 4: 提交版本与文档**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp README.md AGENTS.md CLAUDE.md
git commit -m "docs: document grappling hook no-cooldown toggle"
```

---

## Task 5: 游戏内端到端验证（人工）

- [ ] **Step 1: 构建并部署**

```powershell
cmake --build --preset ninja-msvc-x64 --target deploy
```

- [ ] **Step 2: 依次验证**

1. 进入存档，勾选「爪钩枪无冷却」→ 爪钩枪可连发、无冷却。
2. 取消勾选 → 恢复正常冷却。
3. 退出存档重进 → 偏好保持（勾选状态恢复）且生效（重应用成功）。
4. 切换世界/地图后 → 仍生效（世界就绪重应用）。
5. 同时切换「据点资源共享」开关 → 两项偏好互不覆盖、config.ini 两节都正确持久化。
6. 空闲等待至少 10 秒 → UE4SS 日志无逐帧反射刷屏。
7. 手动改 config.ini 为非法值（如 `NoCooldown=maybe`）→ 失败安全回退为关闭，不崩溃。
