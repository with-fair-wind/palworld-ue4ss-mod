# PalworldEditor 中文 Doxygen 注释设计

## 背景

`PalworldEditor` 已包含物品浏览、背包编辑、帕鲁选择、主动/被动技能编辑、运行时本地化目录以及游戏线程任务队列等功能。当前生产代码没有 Doxygen 注释，阅读者需要通过实现细节推断接口契约、线程约束、对象所有权和失败回退行为。

本次工作只补充中文 Doxygen 注释，不修改任何函数签名、控制流、数据布局或运行行为。

## 目标

- 为 `mods/PalworldEditor/inc` 和 `mods/PalworldEditor/src` 下的全部生产代码建立完整、统一的中文 Doxygen 注释。
- 让调用者无需阅读实现即可理解公开接口的用途、参数、返回值、失败条件和线程要求。
- 让维护者能够理解内部辅助接口、状态字段、同步关系、对象生命周期和 Palworld/UE4SS 领域语义。
- 让 clangd、IDE 提示以及后续可能接入的 Doxygen 文档生成流程能够直接消费这些注释。

## 非目标

- 不修改 `mods/PalworldEditor/tests/skill_editor_tests.cpp`。
- 不修改第三方 `RE-UE4SS`。
- 不添加 `Doxyfile`，不生成 HTML、XML 或其他文档产物。
- 不给局部变量、显而易见的单行语句和普通控制流逐行添加机械式注释。
- 不借注释任务重构、重命名或修复现有实现。

## 覆盖范围

以下 8 个生产代码文件全部纳入：

- `mods/PalworldEditor/inc/game/pal_game.hpp`
- `mods/PalworldEditor/inc/items/item_catalog.hpp`
- `mods/PalworldEditor/inc/skills/pal_skills.hpp`
- `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- `mods/PalworldEditor/inc/skills/skill_editor_service.hpp`
- `mods/PalworldEditor/inc/support/text_encoding.hpp`
- `mods/PalworldEditor/src/dllmain.cpp`
- `mods/PalworldEditor/src/pal_skills.cpp`

每个文件应覆盖以下声明：

- 文件与命名空间；
- 类、结构体、枚举及每个枚举值；
- 自由函数、成员函数、构造函数、回调和导出的 C 接口；
- 全局常量、静态常量、类字段和结构体字段；
- 匿名命名空间中的内部类型、常量和辅助函数。

## 注释格式

### 文件

每个文件使用文件级注释说明职责及主要依赖：

```cpp
/**
 * @file item_catalog.hpp
 * @brief 提供与 Unreal 运行时无关的物品目录整理、标签生成和搜索能力。
 * @details 本文件只处理 UTF-8 字符串和值类型，不持有任何游戏对象。
 */
```

### 类型

类型使用 `@brief` 概括职责，必要时以 `@details` 描述不变量、所有权和线程模型：

```cpp
/**
 * @brief 表示可供界面展示和选择的物品目录项。
 * @details `id` 是写入游戏接口的 Raw ID，`localizedName` 只用于展示和搜索。
 */
struct ItemOption
{
    // ...
};
```

### 函数

函数按实际签名使用以下标签：

- `@param[in]`：只读输入；
- `@param[out]`：由函数写入的输出；
- `@param[in,out]`：同时读取和修改；
- `@return`：返回值语义；
- `@retval`：布尔值或离散结果需要逐值说明时使用；
- `@warning`：调用线程、悬空指针、反射布局等安全约束；
- `@note`：回退策略、性能特征或不明显的实现约定。

公共声明提供完整契约。已有头文件声明的 `.cpp` 定义使用 `@copydoc` 继承契约，并仅补充实现细节；只在 `.cpp` 中存在的内部函数则提供完整注释。

### 字段和常量

简单字段可以使用尾随 Doxygen 注释：

```cpp
std::string id; /**< 写入 Palworld 接口的物品 Raw ID。 */
```

复杂字段使用声明前多行注释，说明以下适用信息：

- 业务含义和有效范围；
- 单位、容量或槽位约束；
- 是否拥有所指对象；
- 何时初始化、失效或刷新；
- 由哪个互斥量保护；
- 原子字段承担的跨线程状态含义。

## 内容原则

### 解释契约而非复述名称

注释优先解释调用前提、输出保证、失败表现以及维护者无法仅从类型推断的领域知识，不写“获取物品”“设置数量”一类没有新增信息的复述。

### 保留技术标识

中文描述中的 C++ 标识符、Raw ID、Unreal 类型、UE4SS 名称和反射路径保留原文，并使用反引号标记，例如 `FName`、`UObject*`、`PalUIUtility:GetItemName`。

### 明确所有权与生命周期

UE4SS/Unreal 裸指针默认作为非拥有观察指针说明。返回指针的接口应注明可能返回 `nullptr` 的条件，以及指针有效性依赖游戏对象生命周期的事实。

### 明确线程和同步

涉及 `on_update`、`on_unreal_init`、游戏线程任务队列、互斥量或原子变量的接口和字段必须说明：

- 哪些操作只能在 Unreal 游戏线程执行；
- 哪些数据可由 UI/回调线程提交；
- 哪些字段由具体互斥量保护；
- 队列是否保证 FIFO；
- 原子状态用于请求、完成还是关闭流程。

### 区分展示数据与写入数据

物品和技能相关注释必须区分：

- 本地化名称：仅用于界面展示和搜索；
- Raw ID：传递给游戏反射接口的稳定标识；
- 槽位索引：修改背包或技能位置时使用的位置标识；
- 缓存标签：可失效、可回退，不替代 Raw ID。

### 记录失败与回退

对反射查找、UObject 扫描、技能替换、字符串转换和本地化名称解析，注释应明确：

- 失败时返回空值、Raw ID、错误结果还是保持原状态；
- 替换失败时是否执行回滚；
- 扫描结果是否只包含当前已加载对象；
- UTF-8 转换失败或输入为空时如何处理。

## 修改边界

源码差异只允许增加或调整注释所需的空白。不得修改：

- 标识符、类型、函数签名和默认参数；
- include 集合和顺序；
- 条件判断、循环、函数调用和返回表达式；
- 字段顺序、初始化值和内存布局；
- 日志文字、UI 文案和反射路径。

若格式化工具因注释长度调整换行，只接受不改变 C++ token 序列的格式变化。

## 验证

完成注释后执行：

1. 人工核对 8 个文件，确认文件、类型、枚举值、接口和字段均有 Doxygen 注释。
2. 检查 Git 差异，确认没有行为性代码改动，测试文件和 `RE-UE4SS` 未变化。
3. 运行 `cmake --build --preset ninja-msvc-x64 --target format-check`。
4. 运行 `cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests`。
5. 运行 `ctest --test-dir build --output-on-failure`。
6. 运行 `cmake --build --preset ninja-msvc-x64 --target tidy-check`。
7. 运行 `git diff --check`。

项目当前没有游戏自动化测试，最终仍需在 Palworld 1.0 与 UE4SS Experimental 环境中进行加载验证；纯注释改动不要求新增游戏内行为用例。
