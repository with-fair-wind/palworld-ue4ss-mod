# 技能目录刷新崩溃与本地化修复设计

## 背景

PalworldEditor v1.4.3 已能解析数字键当前高亮、下一次按 E 会召唤的队伍帕鲁，
但技能目录仍存在三个互相关联的问题：

- 游戏启动后目录可能显示
  `Local player party Holder world context is unavailable`；
- 主动和被动技能选择框都处于不可选择状态；
- 点击“刷新技能列表”会使游戏发生访问冲突并退出。

崩溃转储中的异常为 `0xC0000005`。结合当前 DLL 的 PDB 行号，调用栈落在
`PalSkillGateway::load_catalog()` 的
`UEnum::GetEnumNames()` 调用处。RE-UE4SS 的该函数直接按版本相关的成员偏移读取
`UEnum` 内部数组；当前 Palworld 1.0 experimental 运行时的实际布局与该读取路径
不兼容。

目录未能成功加载后，现有 `SkillCatalogSnapshot::ready` 保持为 `false`。GUI 使用这个
总开关同时禁用主动和被动技能选择框，因此“不能选择技能”与刷新崩溃是同一目录加载
故障的后续表现。当前技能找不到目录映射时只能显示数值或 Raw ID，所以也无法显示
本地化名称。

## 目标

- 刷新技能目录不再调用不安全的 `UEnum::GetEnumNames()`，游戏不崩溃；
- 主动和被动技能选择框能够分别加载、搜索和选择；
- 名称通过游戏本身的 `PalUIUtility` 取得，跟随游戏当前语言；
- 当前游戏语言为中文时显示中文名，同时保留 Raw ID 便于辨认；
- 当前已装备主动技能由数值稳定映射回 Raw ID 和本地化名称；
- 一类目录加载失败时不阻塞另一类目录；
- 刷新暂时失败时继续保留上一份可用目录。

目标版本为 PalworldEditor v1.4.4。

## 方案比较

### 方案一：编译期主动技能 ID 表，加游戏运行时本地化（采用）

从仓库内 Palworld 1.0 UHT dump 的
`UHTHeaderDump/Pal/Public/EPalWazaID.h` 生成一份只包含数值和 Raw ID 的纯 C++
常量表。运行时遍历常量表，并调用
`PalUIUtility::GetWazaName` 取得当前语言名称。

被动技能仍通过
`PalPassiveSkillManager::GetPalAssignablePassiveIDs` 获取可分配 ID，再通过
`PalUIUtility::GetPassiveSkillName` 本地化。

该方案不读取 `UEnum` 内存布局，也不依赖外部运行时文件；ID 与 Palworld 1.0 固定对应，
名称仍由当前游戏资源决定。游戏版本增加技能时需要随 UHT dump 更新常量表。

### 方案二：运行时反射技能 DataTable（不采用）

通过 `PalWazaDatabase` 或 `DataTableFunctionLibrary` 取得行名，再从行结构解析
`WazaType`。该方案可自动覆盖新技能，但需要额外依赖 DataTable、行结构和数据库对象的
加载时序，反射链更长，也引入新的版本布局风险，无法作为此次崩溃修复的最小可靠路径。

### 方案三：随模组附带外部技能 JSON（不采用）

PalworldSaveTools 提供约 375 条主动技能和大量被动技能的静态数据，可作为交叉核对来源。
但这些数据中的名称主要为英文，不能直接满足跟随游戏语言的要求；附带外部数据还会增加
部署、版本同步和许可维护成本。

PalSchema 可辅助核对枚举名称与类型，但同样不能代替游戏的本地化文本接口。

## 数据模型

### 主动技能定义

新增不依赖 Unreal 的主动技能定义：

```cpp
struct ActiveSkillDefinition
{
    std::uint16_t value;
    std::string_view id;
};
```

常量表中的 `id` 使用去掉 `EPalWazaID::` 前缀后的 Raw ID。生成时排除
`None`、`MAX` 等哨兵项，并拒绝重复数值或空 ID。表只负责稳定映射，不保存英文或中文
展示名称。

### 独立目录状态

把单一的 `ready/error` 状态拆成主动、被动两个目录区段：

```cpp
struct SkillCatalogSection
{
    std::vector<SkillOption> skills;
    std::string error;
    bool ready{};
};

struct SkillCatalogSnapshot
{
    SkillCatalogSection passive;
    SkillCatalogSection active;
};
```

刷新结果按区段合并：

- 新区段加载成功时替换旧区段；
- 新区段加载失败、旧区段可用时保留旧技能，并传播最新错误；
- 两个区段互不改变对方的可用状态。

`SkillOption` 保持 Raw ID、本地化名称和可选主动技能数值。展示标签继续使用
`本地化名称 [Raw ID]`；本地化名称为空时回退为 Raw ID。

## 运行时加载流程

### 世界上下文

技能本地化不再复用“当前待出战帕鲁”的本地 Holder 解析结果。两者语义不同：

- Holder 用于确定队伍槽位和编辑目标；
- World Context 只用于让 `PalUIUtility` 找到当前游戏世界和本地化资源。

物品目录已经使用 `PalPlayerInventoryData` 调用
`PalUIUtility::GetItemName` 并能取得本地化名称。技能目录采用同一稳定来源：

```text
FindFirstOf("PalPlayerInventoryData") -> WorldContextObject
```

如果该对象暂时不存在，仍然建立 Raw ID 目录，不调用本地化函数，并在对应区段显示
“本地化暂不可用”的非致命错误。之后点击刷新可重新获取当前语言名称。

### 被动技能

```text
PalPassiveSkillManager.GetPalAssignablePassiveIDs()
  -> 去除空 ID 和重复 ID
  -> PalUIUtility.GetPassiveSkillName(WorldContextObject, ID)
  -> 按“本地化名称 [Raw ID]”排序
```

只要取得了非空的可分配 ID 列表，被动目录即为可选择状态。本地化失败只让单个条目
回退到 Raw ID，不使整个目录失效。

### 主动技能

```text
编译期 ActiveSkillDefinition 表
  -> PalUIUtility.GetWazaName(WorldContextObject, value)
  -> SkillOption{id, localizedName, activeValue}
  -> 按“本地化名称 [Raw ID]”排序
```

加载过程不再查找 `/Script/Pal.EPalWazaID`，也不调用任何直接读取枚举内部存储的 API。
同一份定义表同时建立 `value -> Raw ID` 映射，读取帕鲁当前 `EquipWaza` 时可把原先显示
为 `15`、`124` 的数值转换为技能标签。

## GUI 行为

- “刷新技能列表”仍只设置原子请求，由 `on_update()` 所在游戏线程完成 Unreal 调用；
- 被动选择框只由被动目录状态控制；
- 主动选择框只由主动目录状态控制；
- 搜索同时匹配当前语言名称和 Raw ID；
- 目录错误分别显示为“被动技能目录”和“主动技能目录”，避免一个总错误掩盖真实原因；
- 本地化暂不可用时选择和写入仍可使用 Raw ID；
- 刷新成功后，已经打开但不再存在于新目录中的选择会被清除，避免提交陈旧选项；
- 当前帕鲁切换、目标代数和过期请求保护逻辑保持不变。

## 错误处理与安全边界

- 不捕获或跨线程保存 `UObject*`；
- 所有 `ProcessEvent` 调用继续只发生在游戏线程；
- 不用第一个场景帕鲁、野生帕鲁或任意 Holder 作为目录世界上下文；
- 主动技能定义表异常在纯 C++ 测试和编译期检查中暴露，不在游戏中猜测；
- 本地化失败不影响 Raw ID 目录和技能写入；
- 目录加载失败不清空上一份可用目录；
- 目录刷新不修改当前帕鲁技能，仅更新选择数据和显示标签。

## 组件变更

### `inc/skills/active_skill_definitions.hpp`

- 保存 Palworld 1.0 的主动技能数值与 Raw ID 常量表；
- 提供只读 `std::span` 和按数值查找 Raw ID 的纯函数；
- 记录生成来源和目标游戏版本。

### `inc/skills/skill_catalog.hpp`

- 引入主动、被动独立的目录区段状态；
- 将刷新回退逻辑改为按区段合并；
- 保持标签、搜索、去重等纯函数。

### `src/pal_skills.cpp`

- 删除 `UEnum::GetEnumNames()`、枚举前缀剥离和运行时枚举去重逻辑；
- 从主动技能定义表生成目录和 `activeIds_`；
- 以 `PalPlayerInventoryData` 作为本地化 World Context；
- 分别报告主动和被动目录结果；
- 本地化文本为空时保留 Raw ID。

### `src/dllmain.cpp`

- 每个选择框使用对应目录区段的可用状态；
- 分别显示主动、被动目录错误；
- 刷新后清理已失效的 GUI 选择；
- 保持请求队列、目标代数和游戏线程交接结构不变。

### 版本与文档

- 将 C++ mod 元数据和 GUI 标题更新为 `1.4.4`；
- 更新 README、AGENTS.md 和 CLAUDE.md 中的版本与技能目录说明；
- 说明主动技能 ID 表针对 Palworld 1.0，升级游戏版本后需要根据新 UHT dump 更新。

## 测试策略

### 纯 C++ 测试

- 主动技能定义不含空 ID、哨兵、重复数值或重复 Raw ID；
- 选取若干已知 `EPalWazaID` 验证数值到 Raw ID 的映射；
- 主动技能数值能映射为目录标签，未知值仍安全回退为十进制数值；
- 本地化名称存在时显示 `名称 [Raw ID]`，为空时显示 Raw ID；
- 搜索同时匹配本地化名称和 Raw ID；
- 被动刷新失败只保留并标记被动旧目录，不影响主动新目录；
- 主动刷新失败只保留并标记主动旧目录，不影响被动新目录；
- 首次加载只有一个区段成功时，该区段仍为可选择状态。

### 构建验证

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

### 游戏内验证

1. 中文客户端进入存档，打开 PalworldEditor。
2. 点击“刷新技能列表”，确认游戏不崩溃。
3. 选择数字键当前高亮、下一次按 E 会召唤的队伍帕鲁。
4. 打开被动技能新增或替换界面，确认可以搜索、选择并提交。
5. 打开三个主动技能槽位，确认可以搜索、选择、替换和清空。
6. 确认主动、被动技能显示中文名和 Raw ID，而不是只有数值。
7. 切换游戏语言后重新启动或刷新，确认名称跟随游戏语言变化。
8. 在本地化上下文暂不可用的加载阶段刷新，确认仅显示错误或 Raw ID，游戏不崩溃；
   上下文可用后再次刷新，确认恢复本地化名称。
9. 用数字键切换队伍帕鲁，确认旧目标和旧编辑请求仍会失效。

## 非目标

- 不硬编码简体中文翻译；
- 不修改 `MasteredWaza`、伙伴技能或物种数据；
- 不支持普通多人客户端；
- 不在此次修复中改用 DataTable 或附带完整外部技能数据库；
- 不改变主动、被动技能实际写入与回滚协议。
