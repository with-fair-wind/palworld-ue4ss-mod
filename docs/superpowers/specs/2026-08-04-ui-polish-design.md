# PalworldEditor UI 美化 Design

**日期：** 2026-08-04
**目标版本：** 在当前最新代码（1.6.10，已完成代码重构分支）之上，纯 ImGui 表现层改动，不改变功能逻辑、不升版本号。

## 目标

美化 `PalworldEditor` 的 ImGui 界面，解决当前“一个又高又窄的长条窗口 + ImGui 默认灰色样式”观感差的问题。具体两件事：

1. **重组布局**：用顶部 Tab 分页替换单窗口垂直堆叠，窗口固定默认尺寸、可由玩家缩放，不再“一长条”。
2. **换视觉主题**：应用一套“深色现代”配色（深灰底、强调蓝、圆角、舒适间距），替换 ImGui 默认主题。

## 非目标

- **不改任何功能逻辑**：反射路径、UFunction 调用、Hook、跨线程请求交接、安全停用域、属性/技能/形态/资源共享的业务行为全部不变。
- **不动字体**：当前中文字体显示正常；更换/打包 CJK 字体资源是独立工作，本次不做。
- **不改 `render_*` 函数签名**：各 `*_ui.cpp` 里的 `PalworldEditorMod::render_xxx` 签名保持不变，只在函数体内部做小幅样式调整。
- **不引入第三方 imgui 扩展/皮肤库**：只用 imgui 1.92.1 自带 API。
- **不升版本号**。

## 现状

- 构造函数 `register_tab` 的回调体里：`ImGui::Begin("PalworldEditor v1.6.10", nullptr, ImGuiWindowFlags_AlwaysAutoResize)`，内部依次调用 `render_give_items / render_item_browser / render_inventory / render_base_resource_sharing / render_grapple_no_cooldown / render_pal_editor`，中间用裸 `ImGui::Separator()` 分隔，末尾一个“发现对象”按钮。
- `render_pal_editor` 内部已用 `ImGui::CollapsingHeader` 分了被动 / 主动 / 形态 / 属性四块。
- **无任何 `ImGuiStyle` 定制**：未调用 `StyleColorsDark/Light`、未改 `FrameRounding` 等字段，用的是 imgui 默认主题。
- imgui 版本 1.92.1（`BeginTabBar` / `BeginTabItem` / `BeginChild` / 全部 `ImGuiStyle` 字段均可用）。
- 渲染实现已按功能拆到 `src/{items,skills,pal_stats,pal_identity,base_resource_sharing}/*_ui.cpp`，类声明在 `inc/mod/mod_core.hpp`。

## 设计

### 1. 布局：顶部 Tab 分页

去掉 `ImGuiWindowFlags_AlwaysAutoResize`，改为固定默认尺寸 + 顶部 Tab 栏：

- **窗口尺寸**：渲染前 `ImGui::SetNextWindowSize(ImVec2(580, 640), ImGuiCond_FirstUseEver)`。首次以 580×640 出现，之后 imgui INI 会记住玩家调整的位置与大小。
- **Tab 栏**：`ImGui::BeginTabBar("##editor_tabs", ImGuiTabBarFlags_None)`，下含 4 个 `BeginTabItem`：

| Tab | 内容 | 来源 render 函数（签名不变） |
|-----|------|------------------------------|
| 物品 | 给予物品 + 物品目录 + 背包 | `render_give_items` / `render_item_browser` / `render_inventory` |
| 帕鲁 | 帕鲁编辑器（内部保留 CollapsingHeader：被动 / 主动 / 形态 / 属性） | `render_pal_editor` |
| 据点 | 资源共享 + 爪钩枪无冷却 | `render_base_resource_sharing` / `render_grapple_no_cooldown` |
| 诊断 | 发现对象按钮（预留诊断文本区） | 当前构造函数末尾的“发现对象”按钮 |

- Tab 内各功能块之间用“带标题分组”（见 §3）分隔，不再用裸 `Separator`。
- `render_pal_editor` 内部已有的 `CollapsingHeader` 结构保留，不动。

### 2. 视觉主题：深色现代

新增 `editor_ui::apply_editor_style()`，在渲染窗口前调用一次（mod 加载后、首次渲染前）。实现：先 `ImGui::StyleColorsDark(&style)` 取基底，再覆盖以下字段。

**配色（RGBA 按 hex 推算，alpha 默认 1.0）：**

| ImGuiCol 字段 | 值 |
|---------------|-----|
| WindowBg / ChildBg / PopupBg | `#1f1f1f` |
| Border / Separator | `#3a3a3a` |
| FrameBg | `#2d2d2d` |
| FrameBgHovered | `#383838` |
| FrameBgActive | `#3a3a3a` |
| Button（主操作） | `#3a7afe` |
| ButtonHovered | `#5a8fff` |
| ButtonActive | `#2e6ae0` |
| Header（Tab 选中 / 列表项悬停） | `#3a7afe` |
| HeaderHovered / HeaderActive | `#33415c` |
| Tab（未选中） | `#2d2d2d` |
| TabHovered | `#383838` |
| TabActive | `#323232` |
| Text | `#e0e0e0` |
| TextDisabled | `#888888` |
| ScrollbarBg | `#1f1f1f` |
| ScrollbarGrab | `#3a3a3a` / Hovered `#4a4a4a` |

**圆角：**

| 字段 | 值 |
|------|----|
| WindowRounding | 8 |
| ChildRounding | 6 |
| PopupRounding | 6 |
| FrameRounding | 6 |
| GrabRounding | 6 |
| TabRounding | 6 |
| ScrollbarRounding | 8 |

**间距 / 其它：**

| 字段 | 值 |
|------|----|
| WindowPadding | (12, 12) |
| FramePadding | (8, 4) |
| ItemSpacing | (8, 6) |
| ItemInnerSpacing | (6, 4) |
| IndentSpacing | 18 |
| ScrollbarSize | 13 |
| AntiAliasedLines | true |

> 备注：配色最终以游戏内 imgui 实际渲染为准，实现后可微调色值；上述是首版基线。

### 3. 分组与控件细节

- **带标题分组**：新增 helper `editor_ui::section_header(const char* title)`——渲染一行小标题（`ImGui::Text`）+ 一条全宽分隔线（`ImGui::Separator`），替代各 Tab 内裸 `Separator`，给“给予物品 / 物品目录 / 背包”等块清晰边界。各 `render_*` 内部首个 `Separator` 类元素统一换成 `section_header`。
- **按钮主次分级**：主操作（给予 / 设置数量 / 应用预设 / 确认修改 / 应用属性修改 / 应用形态修改 / 应用工作适应性修改）在渲染前 `PushStyleColor(ImGuiCol_Button, accent)` + `PushStyleColor(ImGuiCol_ButtonHovered/Active, ...)` 套强调蓝，渲染后 `PopStyleColor`；次操作（刷新 / 扫描 / 取消 / 重新检测）保持默认中性色。用一个 RAII 小 helper（如 `scoped_accent_button`）保证 Push/Pop 配对。
- **输入宽度对齐**：物品 ID、数量、各种 `DragInt` / `InputText` 用 `ImGui::SetNextItemWidth` 统一（数值类 ~160，物品 ID ~200），消除当前参差。

### 4. 实现落点

**新增：**
- `inc/mod/editor_ui.hpp`：声明 `apply_editor_style()`、`section_header(const char*)`、`render_main_window(PalworldEditorMod*)`，以及按钮分级 helper（如 `scoped_accent_button`）。
- `src/mod/editor_ui.cpp`：实现上述。
  - `render_main_window` 负责：每帧 `apply_editor_style()` → `SetNextWindowSize(..., ImGuiCond_FirstUseEver)` → `Begin` → `BeginTabBar` → 按 Tab 调用对应 `render_*`（诊断 Tab 调 `render_diagnostics`）→ `EndTabBar` → `End`。
  - 新增 `render_diagnostics(PalworldEditorMod* self)`：渲染“发现对象”按钮（点击置 `self->want_discover_ = true`）+ 预留诊断文本区。

**改动：**
- `src/mod/dllmain.cpp` 构造函数 `register_tab` 回调体：简化为 `editor_ui::render_main_window(self)`。
- `apply_editor_style()` 由 `render_main_window` 在每帧 `Begin` 之前幂等调用一次（imgui style 是进程全局，可能被 UE4SS 或其它 mod 重置，每帧覆盖最稳，开销可忽略）；不在 `on_unreal_init` 单独调用。
- `CMakeLists.txt`：源列表加入 `src/mod/editor_ui.cpp`。
- 各 `*_ui.cpp`（item_ui / skill_ui / base_resource_ui / stat_ui / pal_identity_ui）：把函数体里首个裸 `Separator`/标题换成 `section_header(...)`；主操作按钮套 `scoped_accent_button`；输入控件宽度对齐。**签名不变**。

**不改动：**
- `inc/mod/mod_core.hpp` 的类声明与 `render_*` 声明（除非为按钮 helper 增加自由函数声明，但 helper 放 editor_ui.hpp 即可，不入类）。
- 任何反射、Hook、请求交接、领域逻辑。

## 验收标准

- 构建绿：`format-check + PalworldEditor + PalworldEditorTests + PalworldEditorBaseResourceSharingTests + ctest` 全通过。
- 游戏内冒烟（人工）：
  - 窗口默认 ~580×640，可缩放；首次出现后 imgui 记住位置/大小。
  - 顶部 4 个 Tab 切换正常，各 Tab 内容正确（物品/帕鲁/据点/诊断）。
  - 深色现代主题生效（深灰底、圆角、强调蓝主按钮）。
  - 帕鲁编辑器内 CollapsingHeader 仍可展开/收起，编辑/应用流程功能不变。
  - 给予物品、修改背包数量、资源共享开关、爪钩开关、属性/形态修改等**业务行为与改版前一致**。
  - 发现对象按钮在诊断 Tab 仍能触发诊断扫描。

## 风险与回退

- **imgui style 全局污染**：本 mod 覆盖全局 style 可能影响 UE4SS 自身或其它 mod 的 imgui 外观。缓解：仅在渲染前覆盖一次；若发现冲突，可改为在 `Begin/End` 内用 `PushStyleVar/Color` 局部作用（代价是代码量增加）。首版先用全局覆盖，观察是否有反馈。
- **配色观感**：色值为首版基线，需游戏内实看后微调（不影响功能）。
- **回退**：纯表现层改动；若出问题，revert 本次提交即可恢复，无数据/存档影响。
