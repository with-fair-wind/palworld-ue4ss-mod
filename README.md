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
| **钓鱼圣手** | 默认关闭；咬钩等待归零、捕获进度一次即满，关闭/切图按条件恢复四个原值 |
| **标记点传送** | 默认 F7；一键传送到水平距离最近的自定义地图标记，门控与到达高度可配（waypoint_teleport.ini） |

## 快速开始

需要 PowerShell 7、VS C++ 桌面工作负载、CMake ≥ 3.26、Git 和 Rust stable。
通过显式更新选择最新 UE4SS，普通构建不更新依赖。可直接使用 CMake，也可用
`pwsh scripts/build.ps1 -Test` 自动加载 VS x64 环境并构建、测试。

```powershell
pwsh scripts/setup.ps1 -Update                 # 首次获取或显式更新 UE4SS 及子模块
# 以下命令在 VS x64 开发者环境中执行
cmake --preset ninja-msvc-x64 -DPALWORLD_INSTALL_DIR="F:/Games/Palworld"
cmake --build --preset ninja-msvc-x64          # 构建 UE4SS 和 PalworldEditor
# 完全退出游戏后
cmake --build --preset deploy
```

已有可用 UE4SS 安装时，部署只替换 UE4SS.dll 和本 mod 的 main.dll；不覆盖加载器、适配文件、
用户配置或其他 mod。更新 UE4SS 后使用新的构建目录。

进入游戏后：UE4SS GUI → `PalworldEditor` 页签 → 浮动窗口。详细操作方式见下文架构/已知限制，
完整验证清单见 `AGENTS.md`。

### 更新与部署参数

`setup.ps1` 首次获取上游 main，已有源码时只同步当前提交的递归子模块；
`-Update` 才获取最新提交，也可用 `-Update -Ref <分支/tag/提交>`。
更新前拒绝源码或子模块中的本地修改；子模块跟随父仓库记录的版本，不使用 `--remote`。

默认构建目录为 `build/msvc`。依赖或工具链变化后，保留旧产物并选择新目录：

```powershell
cmake --preset ninja-msvc-x64 -B build/msvc-next -DPALWORLD_INSTALL_DIR="F:/Games/Palworld"
cmake --build build/msvc-next --target UE4SS PalworldEditor
# 完全退出游戏后
cmake --build build/msvc-next --target deploy
```

`-B` 不会修改 preset，后续命令必须使用同一目录。
也可执行 `pwsh scripts/build.ps1 -BuildDirectory build/msvc-next -Test`，
然后用 `pwsh scripts/deploy.ps1 -BuildDirectory build/msvc-next -InstallDirectory 'F:/Games/Palworld'`
复制已有构建结果。CMake 的 `deploy` 会先构建，独立部署脚本不会；
`deploy-ue4ss` 是 `deploy` 的兼容别名。

部署要求已有可用 UE4SS 安装，首次运行时安装及 Palworld 适配由兼容发行包或创意工坊完成。
默认工作目录为 `Pal/Binaries/Win64/ue4ss`。有创意工坊、override.txt 或启动参数重定向时，
配置 `-DPALWORLD_UE4SS_DIR="实际工作目录"`，或向部署脚本传入 `-RuntimeDirectory`。
该目录须同时包含 UE4SS.dll 和 Mods；若使用分离目录的自定义安装，应手动复制到各自实际位置。
首次添加本 mod 时，自行创建 `Mods/PalworldEditor/enabled.txt` 或在现有 mod 配置中启用。

脚本在写入前检查游戏进程、两个源文件和目标文件占用，不改动加载器、布局/签名或配置。
复制不是跨文件原子事务；磁盘错误或中断后应保持游戏关闭，排除故障并重新部署两个 DLL。
不提供自动备份或回滚，需要恢复时部署保留的旧构建产物。调试 PDB 直接使用构建目录中的匹配文件。
替换后必须冷启动验证新日志、GUI 和存档中的基本功能；编译成功不等于最新上游与当前 Palworld 适配兼容。

### VS Code 附加调试

仓库提供 `.vscode` 配置，需要安装推荐的 CMake Tools、clangd 和 Microsoft C/C++ 扩展。先从
VS x64 开发者命令行运行 `code .`，使 VS Code 的构建任务继承 MSVC 环境，然后按以下顺序操作：

1. 游戏退出时运行任务 `CMake: deploy PalworldEditor`，确保部署的 `main.dll` 与
   `build/msvc/Game__Shipping__Win64/bin/PalworldEditor.pdb` 来自同一次构建；
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
  `UE_GAME` 等宏；产物落在 `build/msvc/Game__Shipping__Win64/bin/`；
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
  `revive_timer/`（终端复活计时移除）、`fishing_boost/`（钓鱼圣手）、
  `waypoint_teleport/`（标记传送）、
  `base_resource_sharing/`（跨据点资源共享）、`common/`（跨模块反射原语）、`mod/`（生命周期与 UI 编排）；
- **安全契约**：跨帧不持有 UObject 指针；修改前重查 GUID 与目标/世界代次；LoadMap 清空请求并撤销
  写权限；形态修改只允许收回状态；每项编辑带写后重读验证与失败回滚/安全停用域；
- **资源共享**：只接受同公会、已加载、`Chest` 类型普通仓储；通过原生
  `OnAvailableConcreteModel_ServerInternal` 登记；每帧最多 4 条差量、500 微秒软预算；关闭/切图按
  精确账本恢复（详见 `CLAUDE.md` 的资源共享契约）。

## 已知限制

- 直接写 `StackCount` 绕过游戏复制/通知逻辑：单机可用，多人不可靠；
- 技能编辑与制作/建造材料共享仅支持单人/本地房主；制作/建造共享可用，修理材料共享仍不可用（尚未验证安全的修理入口）；
- 爪钩、捕获覆盖、钓鱼圣手与资源共享开关每次启动默认关闭，不跨进程持久化；
- 为覆盖 UE4SS 延迟回收回调闭包的热卸载窗口，Mod DLL 会固定到进程退出；UE4SS 热重载可重建实例与
  Hook，但替换 DLL 二进制后必须重启游戏；若日志报告卸载清理判定失败（不可恢复损失或恢复重试耗尽）
  或超时，旧实例会保留到进程退出，此时也必须重启游戏，不应继续热重载；
- 不要与 IntegratedStorage、UBIM Lite、BlueprintResearch 等修改相同资源路径的 mod 同时启用；
- 依赖 Palworld 1.0 的 UFunction 参数布局与 UHT dump；游戏更新后需重新生成技能定义表。

## 参考

- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) · [创建 C++ mod](https://docs.ue4ss.com/guides/creating-a-c++-mod.html) · [安装 C++ mod](https://docs.ue4ss.com/dev/guides/installing-a-c++-mod.html)
- [UE4SS Experimental (Palworld)（Steam 创意工坊）](https://steamcommunity.com/workshop/filedetails/?id=3625223587)
- [pwmodding.wiki](https://pwmodding.wiki) · [ItemIDs](https://github.com/KURAMAAA0/PalModding/blob/main/ItemIDs.txt)
- [PalSchema](https://github.com/Okaetsu/PalSchema) · [PalworldSaveTools](https://github.com/deafdudecomputers/PalworldSaveTools)
- [UE4SS C++ 模板](https://github.com/UE4SS-RE/UE4SSCPPTemplate)
- [Palworld SDK 依赖登记与更新核查流程](docs/sdk-dependency-registry.md)
