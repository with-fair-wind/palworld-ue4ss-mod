# 被动技能分类选择器设计

**日期：** 2026-07-27

**目标版本：** PalworldEditor 1.6.5

**状态：** 已完成设计与内部复核，等待用户审阅

## 背景

PalworldEditor 1.6.4 已能从
`PalPassiveSkillManager:GetPalAssignablePassiveIDs` 加载可分配被动技能，并通过
`PalUIUtility:GetPassiveSkillName` 显示当前游戏语言下的名称。新增和替换被动技能共用一个支持搜索的技能
下拉框，但目录项目前只有 Raw ID 和本地化名称，数百个候选混在同一列表中，难以按游戏中的词条品质快速选择。

Palworld 1.0 UHT dump 表明：

- `FPalPassiveSkillDatabaseRow` 包含 `Rank`、`AddPal`、`AddRarePal`、
  `AddWorldTreePal`、`AddMutationPal` 和 `Category`；
- `UPalPassiveSkillManager` 暴露
  `GetSkillData(const FName&, FPalPassiveSkillDatabaseRow&) -> bool`；
- 管理器内部还维护普通、Rare、Rainbow、WorldTree 和 Mutation 等分组 Map；
- `EPalPassiveCategory` 只有 `SortDisplayable` 与 `SortNotDisplayable`，它不是界面所需的品质分类。

因此分类应由 `GetSkillData` 返回的 `Rank` 和 `AddWorldTreePal` 推导，不根据 Raw ID 命名模式猜测，也不直接
读取管理器私有 `TMap` 的内存布局。

## 目标

在“新增被动技能”和“选择替换后的被动技能”工作流中提供两级选择：

1. 第一层选择“全部、普通、稀有、极品、传说、负面”；
2. 第二层只列出当前类别下的技能，并继续支持中文名和 Raw ID 搜索。

新增分类功能必须满足：

- 不改变被动技能写入、重读、差量应用或回滚逻辑；
- 不影响主动技能选择器；
- 不把 Unreal 对象、函数或结构地址保存到下一帧；
- 不在空闲状态或普通 GUI 渲染中查询分类；
- 把新增反射成本分摊到多个 EngineTick，尽最大程度避免帧时间尖峰；
- 单项分类失败不能使整个被动技能目录不可用。

## 非目标

- 不按战斗、工作、移动、元素等效果类型继续细分。
- 不修改 `GetPalAssignablePassiveIDs` 的技能枚举范围，也不额外解包或维护完整静态技能名单。
- 不修改词条预设定义和预设差量写入语义。
- 不增加线程、UFunction Hook、全局 UObject 扫描或常驻逐帧任务。
- 不承诺消除现有目录本地化刷新本身的全部开销；本设计保证新增分类查询不会集中执行。

## 分类模型

### 纯值类型

在 `skill_catalog.hpp` 增加：

```cpp
enum class PassiveSkillCategory {
    normal,
    rare,
    premium,
    legendary,
    negative,
};

struct PassiveSkillMetadata {
    int rank{};
    PassiveSkillCategory category{PassiveSkillCategory::normal};
};
```

`SkillOption` 增加可选的被动元数据：

```cpp
std::optional<PassiveSkillMetadata> passiveMetadata;
```

主动技能的 `passiveMetadata` 始终为空。元数据为空表示分类未知；未知技能保留在“全部”中，但不进入任何具体
分类。

### 分类规则与优先级

分类函数只接受纯值：

```text
AddWorldTreePal == true → 传说
Rank < 0                → 负面
Rank >= 4               → 极品
Rank == 3               → 稀有
Rank 0–2                → 普通
```

世界树标志优先于 Rank，确保世界树技能不会落入极品或负面分类。任何超出已知范围的非负 Rank 按上述区间自然
归类，不为未来新增正等级设置硬上限。

### 界面名称与颜色

界面顺序固定为：

1. 全部；
2. 普通：白色；
3. 稀有：黄色；
4. 极品：蓝色；
5. 传说：紫色；
6. 负面：红色。

“全部”使用默认界面文字颜色。具体颜色使用集中定义的 ImGui 样式常量，保证类别下拉项、第二层技能列表、
已选择技能预览和当前已装备被动技能使用一致颜色；颜色只影响展示，不进入领域模型或筛选逻辑。

## 目录与分类任务

### 总体数据流

```text
现有安全门到期
  → 加载 Raw ID 与本地化名称
  → 立即发布可用于“全部”的目录
  → 为未缓存 Raw ID 建立纯值分类任务
  → 后续 EngineTick 小批调用 GetSkillData
  → 合并成功元数据
  → 分类全部结束后一次性发布最终快照
```

分类期间不逐项发布 GUI 快照。“全部”始终可搜索和选择，具体分类暂时禁用，并显示
“正在加载被动技能分类 n/N”。

### 成功缓存

mod 实例持有 `Raw ID -> PassiveSkillMetadata` 的标准库缓存：

- 同一 Raw ID 成功分类后，本次 mod 运行期间不再调用 `GetSkillData`；
- 手动刷新只为新出现或此前失败的 Raw ID 建立任务；
- 成功缓存不包含 Unreal 地址，可以跨 LoadMap 保留；
- 失败项不进入成功缓存，但同一分类任务内只尝试一次，只有后续手动刷新才重试。

### 增量预算

分类任务只在确有待处理 ID 时运行。每个 EngineTick：

- 解析一次当前帧 Manager 和 `GetSkillData`；
- 最多处理 8 个 Raw ID；
- 从批次开始计时，每处理一项后检查约 500 微秒预算；
- 达到数量或时间上限后立即结束本帧分类；
- 不在每项或每帧输出日志。

时间预算只能限制下一次调用是否开始，不能中断已经进入的单次 `ProcessEvent`。最大数量同时作为硬上限，避免
计时精度或极快调用导致一帧内执行过多查询。

完成或终止时只输出一次汇总日志，包含总数、成功数、未知数、跨越 EngineTick 数和观察到的最大单批耗时，
供游戏内判断是否存在明显帧时间尖峰。

## 安全的 `GetSkillData` 反射

不得在 C++ 中复制完整 `FPalPassiveSkillDatabaseRow` 布局。分类批次使用动态参数缓冲：

1. 通过 `UFunction::GetParmsSize()` 分配零初始化字节缓冲；
2. 调用 `UFunction::InitializeStruct()` 初始化参数及嵌套 `FString`；
3. 反射查找 `SkillName`、`outSkillData` 与返回值属性；
4. 用输入属性的 `CopyCompleteValue` 写入当前 `FName`；
5. 执行 `ProcessEvent`；
6. 返回成功后，从 `outSkillData` 的 `UScriptStruct` 按名称查找 `Rank` 和
   `AddWorldTreePal`，分别通过数值和布尔属性读取；
7. RAII guard 在所有退出路径调用 `DestroyStruct()`。

Manager、UFunction、FProperty 和输出结构地址只在当次 EngineTick/批次内使用，不写入任务或缓存。函数、
参数或字段反射不完整时，本批次报告结构级失败并终止任务，避免用猜测偏移继续读取。

## GUI 交互

### 两级选择器

被动技能进入新增或替换模式后渲染：

```text
类别：[全部 ▼]
技能：[请选择技能 ▼]
```

技能下拉框内部保留现有搜索输入。过滤顺序为：

1. 类别过滤；
2. 已装备 Raw ID 排除；
3. 中文名和 Raw ID 搜索。

“全部”跳过类别过滤，因此也包含元数据未知项。

### 状态规则

- 类别默认为“全部”。
- 新增与替换共用同一个 GUI 类别状态，方便连续编辑。
- 切换类别时清空当前已选择技能，避免确认一个已从当前列表隐藏的旧值。
- 切换类别时保留搜索文本。
- 取消单次新增/替换不重置类别。
- 世界代次、目标代次或完整技能编辑 UI 重置时，类别恢复“全部”。
- 已装备技能列表同样按元数据着色；找不到元数据时使用默认文字颜色。
- 首次分类任务未完成或结构级失败且没有旧分类快照时，具体类别禁用；“全部”保持可用。
- 已经存在完整分类快照时，后续刷新失败保留旧分类和具体类别入口，只显示最新警告。

## 组件边界

### `inc/skills/skill_catalog.hpp`

- 定义 `PassiveSkillCategory` 和 `PassiveSkillMetadata`；
- 提供纯值分类函数；
- 提供类别标签、类别顺序和按类别筛选；
- 扩展 `SkillOption`，但不依赖 Unreal 或 ImGui；
- 保存可测试的分类任务纯值状态和成功缓存合并规则。

### `inc/skills/pal_skills.hpp` 与 `src/pal_skills.cpp`

- 增加一批 Raw ID 的元数据读取接口；
- 每批只在游戏线程解析 Manager、函数和属性；
- 返回纯值成功项和结构级/单项错误，不保存 Unreal 地址；
- 继续由现有 `load_catalog()` 负责 ID 与本地化名称。

### `src/dllmain.cpp`

- 在目录刷新后启动或更新分类任务；
- 在 EngineTick 中按预算推进；
- 完成后合并并发布目录快照；
- 保存 GUI 当前类别并渲染两级选择器；
- 集中维护 ImGui 类别颜色；
- LoadMap 前取消未完成任务。

## 错误处理

| 情况 | 行为 |
|---|---|
| Manager 或 `GetSkillData` 不可用 | 终止本轮分类；没有旧分类时只保留“全部”，否则保留旧分类并显示警告 |
| 参数属性或输出结构字段缺失 | 终止本轮分类，不尝试硬编码偏移 |
| 单个 ID 返回 `false` | 标记为未知，继续下一项 |
| 分类任务跨帧时发生 LoadMap | 立即取消任务，不发布旧世界的中间结果 |
| 手动刷新 | 保留成功缓存，只重试新增项和此前失败项 |
| 目录刷新失败并回退旧目录 | 保留旧目录已有分类，不启动基于失败结果的新任务 |

分类错误独立于被动技能目录错误。已有 Raw ID 和名称可用时，分类失败不能把
`SkillCatalogSection::ready` 改为 `false`。

## 测试

### 纯 C++ 测试

在 `PalworldEditorTests` 中增加：

- 世界树标志覆盖所有 Rank；
- 负 Rank、0–2、3、4 及更高 Rank 的映射；
- 未知元数据只出现在“全部”；
- 类别、排除集合和搜索词的组合过滤；
- 类别切换要求调用方清空旧选择的状态转换；
- 分类任务每批不超过硬数量上限；
- 成功缓存不会重复生成查询；
- 单项失败不阻断后续项；
- 手动刷新只重新排队失败和新增项；
- 旧目录回退保留已有元数据。

反射实现由 DLL 编译覆盖，具体 UFunction 行为仍需游戏内验证。

### 游戏内验证

1. 连续冷启动并进入存档，确认目录安全门行为不变且不崩溃。
2. 分类期间“全部”可用，具体类别显示加载进度并保持禁用。
3. 分类完成后验证五类内容和文字颜色。
4. 已知世界树技能必须归入“传说”，Rank 4 非世界树技能归入“极品”。
5. 新增与替换都使用相同两级选择器，搜索与已装备排除仍生效。
6. 手动刷新不重复读取全部成功元数据。
7. LoadMap 中途取消任务，进入新世界后目录安全门到期再继续。
8. 观察一次性汇总日志的最大单批耗时，并用游戏帧时间工具确认没有由分类新增的明显尖峰。
9. 回归被动新增、替换、删除、四词条预设和主动技能编辑。

## 文档与版本

- `ModVersion`、加载日志和窗口标题更新为 `1.6.5`；
- README 说明两级分类、颜色、加载中行为和分类数据来源；
- AGENTS.md 与 CLAUDE.md 记录增量任务、反射字段、缓存和验证要求；
- 不修改资源共享版本契约和配置格式。

## 验收标准

- 被动新增和替换均出现“全部、普通、稀有、极品、传说、负面”类别下拉框；
- 具体分类内容符合确认的 Rank/WorldTree 规则；
- 全部技能仍可通过中文名或 Raw ID 找到；
- 分类失败不会禁用“全部”或技能写入；
- 新增查询只在活动分类任务中按批次运行，空闲时没有相关反射工作；
- 无 UObject、UFunction、FProperty 或结构地址跨 EngineTick 保存；
- 仓库规定的格式检查、两个测试 target、CTest 和 `git diff --check` 全部通过；
- 游戏内未观察到由分类任务引入的明显丢帧。
