# PalworldEditor 1.7.0 — Palworld 物品、帕鲁技能与据点资源编辑器

基于 [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) 的 C++23 mod，为 Palworld 1.0 提供游戏内
物品/技能/属性编辑与可选的同公会跨据点材料共享。所有操作通过 UE4SS GUI 的 ImGui 窗口完成
（Palworld 的 F10 控制台不可用）。

## 功能

| 功能 | 说明 |
|---|---|
| **给予物品 / 物品浏览器** | 运行时物品目录 + 本地化搜索，给予物品与修改主背包数量 |
| **目标帕鲁** | 数字键高亮下一次按 E 会召唤的队伍帕鲁，点击"选择当前帕鲁"锁定 |
| **主动/被动技能** | 编辑 3 个主动槽位与最多 4 个被动词条；被动支持 普通/稀有/极品/传说/负面 分类与四词条预设 |
| **属性修改** | 等级、个体值、四项帕鲁之魂强化、浓缩星级、性别、13 类工作适应性永久加成、亲密度；差量提交 + 写后验证 |
| **形态修改** | Alpha、Lucky、觉醒三个独立开关；需收回帕鲁，失败整笔回滚 |
| **远程终端** | 按键触发跨据点终端，圈内判定与战斗中禁用可配（默认关闭） |
| **据点资源共享** | 同公会跨据点制作/建造材料共享（默认关闭，仅单人/本地房主） |
| **爪钩枪无冷却** | 默认关闭；按对象精确覆盖冷却并在关闭/切图时恢复原值 |
| **捕获限制覆盖** | 默认关闭；"解锁不可捕获目标"（含人类 NPC）与"强制 100% 成功率"两个独立开关，投球调用期间临时覆盖、返回后恢复原值 |
| **终端复活计时移除** | 默认关闭；可逆清零 PalBoxReviveTime，关闭/切图恢复原值 |
| **标记点传送** | 默认 F7；一键传送到水平距离最近的自定义地图标记，门控与到达高度可配（waypoint_teleport.ini） |

## 快速开始

需要 VS 2022（C++ 桌面开发）、CMake ≥ 3.22、Git、Rust stable，以及游戏内装好
**UE4SS Experimental (Palworld)** + PalSchema。所有命令在 **VS x64 开发者环境**中运行：

```powershell
pwsh scripts/setup.ps1                                     # 首次：克隆 RE-UE4SS
$env:PALWORLD_INSTALL_DIR = "F:\...\Palworld"              # 可选；配置前设置
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
ctest --test-dir build --output-on-failure                 # 纯 C++ 契约测试
cmake --build --preset ninja-msvc-x64 --target deploy      # 部署到游戏（需先退出游戏）
```

进入游戏后：UE4SS GUI → `PalworldEditor` 页签 → 浮动窗口。详细操作方式见下文架构/已知限制，
完整验证清单见 `AGENTS.md`。

### VS Code 附加调试

仓库提供 `.vscode` 配置，需要安装推荐的 CMake Tools、clangd 和 Microsoft C/C++ 扩展。先从
VS x64 开发者命令行运行 `code .`，使 VS Code 的构建任务继承 MSVC 环境，然后按以下顺序操作：

1. 游戏退出时运行任务 `CMake: deploy PalworldEditor`，确保部署的 `main.dll` 与
   `build/Game__Shipping__Win64/bin/PalworldEditor.pdb` 来自同一次构建；
2. 启动 Palworld，等待 UE4SS 输出 `PalworldEditor loaded`；
3. 在 VS Code 的“运行和调试”中选择 `Attach to Palworld (MSVC)`，按 F5，然后选择
   `Palworld-Win64-Shipping.exe`（不同发行渠道名称可能略有差异）；
4. 在 `mods/PalworldEditor/src/` 中设置断点。调试器会从构建输出目录加载 PDB，无需把 PDB 复制到
   游戏目录。

当前 triplet 使用 `/Zi /O2`：附加、调用栈和源码断点可用，但优化可能导致部分局部变量不可见、语句
断点移动或被内联。游戏运行时会锁定已部署的 `main.dll`，修改代码后需退出游戏再重新部署。

## 架构

### 通用架构（与 Palworld 无关的部分）

- **Super-build**：根 `CMakeLists.txt` = `add_subdirectory(RE-UE4SS)` + `add_subdirectory(mods)`；
  RE-UE4SS 提供 `UE4SS` 静态库 target（头文件、编译宏、C++23）；每个 mod 是链接它的 `SHARED` 库；
- **UE4SS triplet**：preset 显式设 `CMAKE_BUILD_TYPE=Game__Shipping__Win64` 以驱动
  `UE_GAME` 等宏；产物落在 `build/Game__Shipping__Win64/bin/`；
- **三层分层**：纯值领域层（`inc/`，只依赖标准库，可单测）→ 游戏线程适配层（`src/<feature>/`，反射
  读写只在 EngineTick 或 UFunction 回调内）→ ImGui UI 层（`src/<feature>/*_ui.cpp`，只传标准库快照/原子
  请求/互斥锁参数）；
- **入口点契约**：`PalworldEditorMod : RC::CppUserModBase` 导出 `start_mod()` / `uninstall_mod()`；
  日志走 `RC::Output::send`。部署契约：`ue4ss/Mods/<ModName>/dlls/main.dll` + 空 `enabled.txt`。

### 针对 Palworld 的业务架构

- **反射路径**：全部通过 `/Script/Pal.*` UFunction/UProperty 反射调用，是 Palworld 专用实现；
- **业务模块**：`game/`（背包/队伍帕鲁反射）、`items/`（物品目录）、`skills/`（技能目录、分类、
  编辑、预设、目标锁定；主动技能数值来自 UHT dump 生成的表）、`pal_stats/`（属性编辑 +
  `SaveParameter` 适配）、`pal_identity/`（Alpha/Lucky/觉醒）、`grappling_hook/`（冷却覆盖）、
  `capture_override/`（临时捕获覆盖）、`pal_remote_palbox/`（远程终端）、`pal_revive/`（队伍复活）、
  `revive_timer/`（终端复活计时移除）、`waypoint_teleport/`（标记传送）、
  `base_resource_sharing/`（跨据点资源共享）、`common/`（跨模块反射原语）、`mod/`（生命周期与 UI 编排）；
- **安全契约**：跨帧不持有 UObject 指针；修改前重查 GUID 与目标/世界代次；LoadMap 清空请求并撤销
  写权限；形态修改只允许收回状态；每项编辑带写后重读验证与失败回滚/安全停用域；
- **资源共享**：只接受同公会、已加载、`Chest` 类型普通仓储；通过原生
  `OnAvailableConcreteModel_ServerInternal` 登记；每帧最多 4 条差量、500 微秒软预算；关闭/切图按
  精确账本恢复（详见 `CLAUDE.md` 的资源共享契约）。

## 已知限制

- 直接写 `StackCount` 绕过游戏复制/通知逻辑：单机可用，多人不可靠；
- 技能编辑与制作/建造材料共享仅支持单人/本地房主；制作/建造共享可用，修理材料共享仍不可用（尚未验证安全的修理入口）；
- 爪钩、捕获覆盖与资源共享开关每次启动默认关闭，不跨进程持久化；
- 为覆盖 UE4SS 延迟回收回调闭包的热卸载窗口，Mod DLL 会固定到进程退出；UE4SS 热重载可重建实例与
  Hook，但替换 DLL 二进制后必须重启游戏；若日志报告卸载清理失败或超时，旧实例会保留到进程退出，
  此时也必须重启游戏，不应继续热重载；
- 不要与 IntegratedStorage、UBIM Lite、BlueprintResearch 等修改相同资源路径的 mod 同时启用；
- 依赖 Palworld 1.0 的 UFunction 参数布局与 UHT dump；游戏更新后需重新生成技能定义表。

## 参考

- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) · [创建 C++ mod](https://docs.ue4ss.com/guides/creating-a-c++-mod.html) · [安装 C++ mod](https://docs.ue4ss.com/dev/guides/installing-a-c++-mod.html)
- [UE4SS Experimental (Palworld)（Steam 创意工坊）](https://steamcommunity.com/workshop/filedetails/?id=3625223587)
- [pwmodding.wiki](https://pwmodding.wiki) · [ItemIDs](https://github.com/KURAMAAA0/PalModding/blob/main/ItemIDs.txt)
- [PalSchema](https://github.com/Okaetsu/PalSchema) · [PalworldSaveTools](https://github.com/deafdudecomputers/PalworldSaveTools)
- [UE4SS C++ 模板](https://github.com/UE4SS-RE/UE4SSCPPTemplate)
- [Palworld SDK 依赖登记与更新核查流程](docs/sdk-dependency-registry.md)
