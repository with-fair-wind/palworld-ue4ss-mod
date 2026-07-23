# Clang 格式化与静态检查工具设计

日期：2026-07-23
状态：已获用户批准，等待实施计划

## 背景

当前仓库使用：

- 基于 `Microsoft` 的 `.clang-format`，采用 Allman 大括号、4 空格和 120 列；
- 面向现代 C++ 与 UE4SS 互操作场景定制的 `.clang-tidy`；
- 读取 `build/compile_commands.json` 并屏蔽 `RE-UE4SS/` 第三方诊断的 `.clangd`；
- 不包含手动运行全项目 clang-format 或 clang-tidy 的 CMake target。

本次修改的目标是把 mod 自有代码切换到基于 Google 的格式规范，并参考
[with-fair-wind/ModernCpp](https://github.com/with-fair-wind/ModernCpp) 与
[with-fair-wind/1mZGG](https://github.com/with-fair-wind/1mZGG) 的 clang-format、clang-tidy
及 CMake 工具集成方式，同时保留本仓库作为 UE4SS super-build 的边界。

## 目标

1. clangd 在编辑器中使用基于 Google 的统一格式。
2. clang-tidy 对 mod 自有代码提供现代 C++、安全性、性能和可读性诊断。
3. 提供跨平台、手动触发的 CMake 工具 target。
4. 所有格式化与静态检查均限定在 `mods/`，不修改或分析 `RE-UE4SS/`。
5. 普通配置、构建和部署流程不依赖 clang-format 或 clang-tidy。

## 非目标

- 不格式化、修复或检查 `RE-UE4SS/`、构建目录及其他第三方文件。
- 不把 clang-format 或 clang-tidy 挂到默认构建。
- 不通过 `CMAKE_CXX_CLANG_TIDY` 对每个编译 target 强制执行检查。
- 不在本次修改中引入 CI 工作流。
- 不强制 UE4SS/Unreal API 遵循本仓库的命名规则。

## 方案选择

采用“参考配置的 UE4SS 适配版”：

- 不原样复制 1mZGG，因为其 Qt 工程目录、命名和检查过滤规则不适用于本仓库；
- 不只修改 `.clang-format`，因为这无法提供可复现的命令行检查入口；
- 吸收两个参考仓库的 Google 格式、检查集合及跨平台 CMake 驱动脚本结构；
- 保留本仓库针对 UE4SS 反射、裸指针、类型转换和第三方代码所需的豁免。

## `.clang-format` 设计

根目录 `.clang-format` 改为：

- `BasedOnStyle: Google`
- `Language: Cpp`
- `Standard: Latest`
- 4 空格缩进，禁止 Tab
- 100 列限制
- `BreakBeforeBraces: Attach`
- 只允许空函数和短 lambda 保持单行
- 指针及引用左对齐
- 构造函数初始化列表和继承列表在冒号前换行
- include 使用 `Regroup`，依次组织标准库、尖括号第三方头和本地双引号头
- 自动补充命名空间结束注释
- 文件末尾保留换行

新配置以用户提供的 `D:/code/qt/1mZGG/.clang-format` 为主体，与项目 C++23 设置相匹配。

实施时会对 Git 跟踪且位于 `mods/` 的现有 C/C++ 文件运行一次格式化。这会产生预期的一次性大括号、
缩进和换行变更，但不会触及 vendored 依赖。

## `.clang-tidy` 设计

启用以下检查族：

- `bugprone-*`
- `cert-*`
- `clang-analyzer-*`
- `concurrency-*`
- `cppcoreguidelines-*`
- `misc-*`
- `modernize-*`
- `performance-*`
- `portability-*`
- `readability-*`

继续禁用与 UE4SS/Unreal 互操作或现有代码约束冲突的高噪音规则，包括但不限于：

- 尾置返回类型；
- 标识符长度和命名；
- 魔数；
- C 数组及指针算术；
- `reinterpret_cast`、`const_cast` 等必要的底层类型转换；
- 裸指针所有权及全局状态；
- 认知复杂度与疑似参数交换等当前不适合强制执行的规则。

`HeaderFilterRegex` 必须只匹配 `mods/` 中的项目头文件。`WarningsAsErrors` 默认留空，
`FormatStyle` 使用 `file`，修复后读取根目录 `.clang-format`。规则应带有注释，说明豁免是 UE4SS
适配而非普遍编码建议。

## `.clangd` 设计

保留现有：

- `CompilationDatabase: build/`
- MSVC 目标三元组补充
- inlay hints 与 hover 设置
- 对 `RE-UE4SS/` 的诊断和未使用 include 抑制

把第三方目录的诊断抑制值从类似正则的 `'.*'` 修正为 clangd 配置规范支持的 `'*'`。

clangd 的格式化请求会自动向上查找 `.clang-format`，clang-tidy 设置也会与 `.clang-tidy`
合并，因此不在 `.clangd` 中重复维护格式规则或检查列表。

## CMake 工具设计

在根 CMake 中引入独立工具模块和两个跨平台 `cmake -P` 驱动脚本。仅当对应可执行文件存在时注册：

- `format`：对范围内文件运行 `clang-format -i --style=file`
- `format-check`：运行 `clang-format --dry-run --Werror --style=file`
- `tidy`：对范围内编译单元运行 `clang-tidy --fix`
- `tidy-check`：只读运行 clang-tidy

工具发现应优先查找带版本后缀的 clang 工具，再回退到无后缀名称。找不到工具时只输出状态信息，
不能令 CMake 配置失败。

所有 target 都是普通 `add_custom_target`，不使用 `ALL`，因此不会影响普通构建和部署。

增加一个默认关闭的 CMake 选项，用于控制 `tidy-check` 是否传递
`--warnings-as-errors=*`。`tidy` 修复模式不把诊断自动升级为错误。

### 格式化文件范围

格式化脚本通过 `git ls-files` 获取 Git 跟踪的 C/C++ 文件，然后统一路径分隔符并只保留 `mods/`
前缀。支持 `.c`、`.cc`、`.cpp`、`.cxx`、`.h`、`.hpp`、`.hxx`。这样：

- 不会修改未跟踪的导出数据或用户临时文件；
- 不会进入 `RE-UE4SS/`；
- Windows、Linux 和 macOS 行为一致。

### 静态检查范围

tidy 脚本要求当前构建目录存在 `compile_commands.json`，从数据库中筛选位于 `mods/`
且后缀为 `.cc`、`.cpp` 或 `.cxx` 的翻译单元。头文件通过这些翻译单元和 `HeaderFilterRegex`
参与检查。

如同版本的 `run-clang-tidy` 可用，可使用其并行能力；否则逐个或批量调用 `clang-tidy`
作为跨平台回退。任何路径筛选都必须在规范化分隔符后完成。

## 错误处理

- clang-format/clang-tidy 不存在：相关 target 不注册，配置继续成功。
- 非 Git checkout：format target 不注册，避免依赖不存在的文件清单。
- `compile_commands.json` 不存在：运行 tidy target 时给出明确错误，提示先执行 preset 配置。
- 范围内没有文件：输出状态并成功返回。
- clang 工具返回非零：对应手动 target 失败并保留原始诊断。

## 验证

实施完成后至少执行：

1. 验证 `.clang-format` 可由当前 clang-format 解析；
2. 使用 `format` 格式化 `mods/`，再运行 `format-check`；
3. 重新执行 `cmake --preset ninja-msvc-x64`，确认工具缺失时也可正常配置；
4. 构建 `MyPalMod` 和测试 target；
5. 运行现有 CTest；
6. 在 clang-tidy 可用且编译数据库存在时运行 `tidy-check`；
7. 检查 Git diff，确认没有修改 `RE-UE4SS/` 或用户已有的非任务文件。

如果本机缺少 clang-format/clang-tidy，应明确记录未执行的验证项，不以未运行冒充通过。
