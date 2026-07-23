# Clang Tooling Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 mod 自有 C++ 代码切换到 Google 风格，并提供仅手动触发、严格限定到 `mods/` 的 clang-format 与 clang-tidy CMake target。

**Architecture:** 根目录配置文件继续由 clangd 自动发现；根 CMake 只加载一个专用工具模块，工具模块负责可执行文件发现和 target 注册，两个独立 `cmake -P` 脚本负责跨平台文件筛选与执行。格式化文件来自 Git 跟踪清单，静态检查翻译单元来自 `build/compile_commands.json`，两条路径都只接受 `mods/`。

**Tech Stack:** C++23、clangd、clang-format 22、clang-tidy 22、CMake ≥ 3.22、Ninja、MSVC/clang-cl 编译数据库、PowerShell 验证命令

## Global Constraints

- `.clang-format` 必须基于 `Google`，4 空格、100 列、`Attach` 大括号、左对齐指针和引用。
- `.clang-tidy` 必须保留 UE4SS/Unreal 互操作所需豁免，不能强制第三方 API 改名或消除必要的底层转换。
- `format`、`format-check`、`tidy`、`tidy-check` 都是手动 target，不能使用 `ALL`。
- 不能设置全局 `CMAKE_CXX_CLANG_TIDY`。
- 所有批量格式化与静态检查只处理 `mods/`，不能触及 `RE-UE4SS/`。
- clang-format 或 clang-tidy 缺失时，CMake 配置仍必须成功。
- `tidy-check` 默认不把警告升级为错误；仅由 `PALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON` 显式开启。
- 保留用户现有的 `.gitignore` 修改以及未跟踪的 `AGENTS.md`、`UHTHeaderDump.7z`，不得暂存或改写。
- 所有构建命令在 VS 2022 x64 开发者环境中运行，使用 `ninja-msvc-x64` preset。

---

## File Structure

- Modify: `.clang-format` — clangd 与命令行统一使用的 Google 格式规则。
- Modify: `.clang-tidy` — mod 自有代码的静态检查集合和 UE4SS 适配豁免。
- Modify: `.clangd` — 保留编译数据库配置，修正第三方诊断抑制值。
- Modify: `CMakeLists.txt` — 把根目录 `cmake/` 加入模块路径并加载工具模块。
- Create: `cmake/PalworldTooling.cmake` — 查找 LLVM 工具并注册四个手动 target。
- Create: `cmake/RunClangFormat.cmake` — 从 Git 跟踪清单筛选 `mods/` 文件并格式化或检查。
- Create: `cmake/RunClangTidy.cmake` — 从编译数据库筛选 `mods/` 翻译单元并检查或修复。
- Modify: `mods/MyPalMod/src/dllmain.cpp` — 仅应用新格式。
- Modify: `mods/MyPalMod/src/item_database.h` — 仅应用新格式。
- Modify: `mods/MyPalMod/src/pal_game.hpp` — 仅应用新格式。
- Modify: `mods/MyPalMod/src/pal_skills.cpp` — 仅应用新格式。
- Modify: `mods/MyPalMod/src/pal_skills.hpp` — 仅应用新格式。
- Modify: `mods/MyPalMod/src/skill_catalog.hpp` — 仅应用新格式。
- Modify: `mods/MyPalMod/src/skill_editor_service.hpp` — 仅应用新格式。
- Modify: `mods/MyPalMod/src/text_encoding.hpp` — 仅应用新格式。
- Modify: `mods/MyPalMod/tests/skill_editor_tests.cpp` — 仅应用新格式。
- Modify: `README.md` — 记录四个手动工具 target 与前置条件。

---

### Task 1: 切换 clangd 使用的格式与检查配置

**Files:**
- Modify: `.clang-format`
- Modify: `.clang-tidy`
- Modify: `.clangd`

**Interfaces:**
- Consumes: clangd 对根目录 `.clang-format`、`.clang-tidy`、`.clangd` 的自动发现机制。
- Produces: `BasedOnStyle: Google` 格式契约、只检查 `mods/` 的 tidy 契约、`RE-UE4SS/` 的 `'*'` 诊断抑制。

- [ ] **Step 1: 运行配置契约检查并确认旧配置不满足目标**

Run:

```powershell
rg -n "^BasedOnStyle: Google$|^Standard: Latest$|^BreakBeforeBraces: Attach$" .clang-format
rg -n "concurrency-\*" .clang-tidy
rg -n "Suppress: \['\*'\]" .clangd
```

Expected:

- 三条命令至少各有一条返回退出码 `1`；
- 当前 `.clang-format` 仍显示 Microsoft/Allman；
- 当前 `.clangd` 使用 `Suppress: ['.*']`。

- [ ] **Step 2: 用 Google 适配配置完整替换 `.clang-format`**

Write exactly:

```yaml
---
# Google C++ style adapted for this C++23 UE4SS mod.
# clangd and the manual CMake targets both discover this file automatically.

BasedOnStyle: Google
Language: Cpp
Standard: Latest
InsertNewlineAtEOF: true

IndentWidth: 4
TabWidth: 4
UseTab: Never
ColumnLimit: 100
AccessModifierOffset: -4

AllowShortFunctionsOnASingleLine: Empty
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
AllowShortLambdasOnASingleLine: All

BreakBeforeBraces: Attach
BreakConstructorInitializers: BeforeColon
BreakInheritanceList: BeforeColon

DerivePointerAlignment: false
PointerAlignment: Left
ReferenceAlignment: Left

SpaceAfterTemplateKeyword: true
SpaceBeforeCpp11BracedList: false
SpaceBeforeRangeBasedForLoopColon: true

IncludeBlocks: Regroup
IncludeCategories:
  - Regex: '^<[a-z][a-z0-9_]*>$'
    Priority: 1
  - Regex: '^<.*\.h(pp)?>$'
    Priority: 2
  - Regex: '^".*"$'
    Priority: 3
SortIncludes: CaseSensitive

FixNamespaceComments: true
NamespaceIndentation: None
```

- [ ] **Step 3: 用 UE4SS 适配配置完整替换 `.clang-tidy`**

Write exactly:

```yaml
---
# Static analysis for the mod's own C++23 sources.
# The disabled checks are deliberate UE4SS/Unreal interop exceptions.

Checks: >
  -*,
  bugprone-*,
  cert-*,
  clang-analyzer-*,
  concurrency-*,
  cppcoreguidelines-*,
  misc-*,
  modernize-*,
  performance-*,
  portability-*,
  readability-*,
  -bugprone-easily-swappable-parameters,
  -bugprone-narrowing-conversions,
  -cert-err58-cpp,
  -cppcoreguidelines-avoid-c-arrays,
  -cppcoreguidelines-avoid-magic-numbers,
  -cppcoreguidelines-avoid-non-const-global-variables,
  -cppcoreguidelines-non-private-member-variables-in-classes,
  -cppcoreguidelines-owning-memory,
  -cppcoreguidelines-pro-bounds-array-to-pointer-decay,
  -cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
  -cppcoreguidelines-pro-bounds-constant-array-index,
  -cppcoreguidelines-pro-bounds-pointer-arithmetic,
  -cppcoreguidelines-pro-type-cstyle-cast,
  -cppcoreguidelines-pro-type-const-cast,
  -cppcoreguidelines-pro-type-member-init,
  -cppcoreguidelines-pro-type-reinterpret-cast,
  -cppcoreguidelines-pro-type-static-cast-downcast,
  -cppcoreguidelines-special-member-functions,
  -misc-include-cleaner,
  -misc-no-recursion,
  -misc-non-private-member-variables-in-classes,
  -modernize-avoid-c-arrays,
  -modernize-use-designated-initializers,
  -modernize-use-nodiscard,
  -modernize-use-trailing-return-type,
  -performance-no-int-to-ptr,
  -portability-avoid-pragma-once,
  -readability-function-cognitive-complexity,
  -readability-identifier-length,
  -readability-identifier-naming,
  -readability-implicit-bool-conversion,
  -readability-magic-numbers,
  -readability-suspicious-call-argument

WarningsAsErrors: ''
HeaderFilterRegex: '(^|.*[/\\])mods[/\\].*\.(h|hpp|hxx)$'
FormatStyle: file
UseColor: true

CheckOptions:
  - key: readability-function-size.LineThreshold
    value: '160'
  - key: readability-function-size.StatementThreshold
    value: '80'
  - key: readability-function-size.ParameterThreshold
    value: '7'
  - key: modernize-use-nullptr.NullMacros
    value: 'NULL'
  - key: modernize-loop-convert.MinConfidence
    value: reasonable
  - key: modernize-use-auto.MinTypeNameLength
    value: '5'
  - key: bugprone-argument-comment.StrictMode
    value: '1'
```

- [ ] **Step 4: 修正 `.clangd` 的第三方诊断抑制**

Apply this exact change:

```diff
 Diagnostics:
-  Suppress: ['.*']
+  Suppress: ['*']
   UnusedIncludes: None
```

同时把紧邻注释中的 “Suppress regex” 改为 “Suppress wildcard”，避免继续把该字段描述成正则。

- [ ] **Step 5: 验证三份配置能够被 LLVM 22 解析**

Run:

```powershell
clang-format --style=file --dump-config mods/MyPalMod/src/pal_skills.cpp > $null
clang-tidy --config-file=.clang-tidy --dump-config mods/MyPalMod/src/pal_skills.cpp -- -std=c++23 > $null
rg -n "^BasedOnStyle: Google$|^Standard: Latest$|^BreakBeforeBraces: Attach$" .clang-format
rg -n "concurrency-\*" .clang-tidy
rg -n "Suppress: \['\*'\]" .clangd
```

Expected:

- 所有命令退出码为 `0`；
- 不出现 YAML 或未知配置键错误；
- 三条 `rg` 分别命中预期配置。

- [ ] **Step 6: 提交配置变更**

```powershell
git add .clang-format .clang-tidy .clangd
git commit -m "style: adopt Google clang tooling config"
```

Expected: commit 只包含上述三个文件。

---

### Task 2: 增加 format target 并格式化 mod 自有代码

**Files:**
- Modify: `CMakeLists.txt`
- Create: `cmake/PalworldTooling.cmake`
- Create: `cmake/RunClangFormat.cmake`
- Modify: `mods/MyPalMod/src/dllmain.cpp`
- Modify: `mods/MyPalMod/src/item_database.h`
- Modify: `mods/MyPalMod/src/pal_game.hpp`
- Modify: `mods/MyPalMod/src/pal_skills.cpp`
- Modify: `mods/MyPalMod/src/pal_skills.hpp`
- Modify: `mods/MyPalMod/src/skill_catalog.hpp`
- Modify: `mods/MyPalMod/src/skill_editor_service.hpp`
- Modify: `mods/MyPalMod/src/text_encoding.hpp`
- Modify: `mods/MyPalMod/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: 根目录 `.clang-format`；Git 跟踪文件清单。
- Produces: CMake target `format` 和 `format-check`；变量 `PALWORLD_CLANG_FORMAT_EXECUTABLE`；只接受 `mods/` 前缀的格式化脚本。

- [ ] **Step 1: 证明 format target 当前不存在**

Run:

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target format-check
```

Expected: 配置成功，但第二条命令以 “unknown target 'format-check'” 或等价信息失败。

- [ ] **Step 2: 创建 `cmake/RunClangFormat.cmake`**

Write exactly:

```cmake
if(NOT CLANG_FORMAT)
    message(FATAL_ERROR "RunClangFormat.cmake: CLANG_FORMAT is not set")
endif()
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "RunClangFormat.cmake: GIT_EXECUTABLE is not set")
endif()
if(NOT SOURCE_DIR)
    message(FATAL_ERROR "RunClangFormat.cmake: SOURCE_DIR is not set")
endif()
if(NOT MODE STREQUAL "fix" AND NOT MODE STREQUAL "check")
    message(FATAL_ERROR "RunClangFormat.cmake: MODE must be 'fix' or 'check'")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" ls-files --
        "*.c" "*.cc" "*.cpp" "*.cxx" "*.h" "*.hpp" "*.hxx"
    OUTPUT_VARIABLE PALWORLD_FORMAT_FILES
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)

if(PALWORLD_FORMAT_FILES STREQUAL "")
    message(STATUS "No tracked C/C++ files found; nothing to format.")
    return()
endif()

string(REPLACE "\r\n" "\n" PALWORLD_FORMAT_FILES "${PALWORLD_FORMAT_FILES}")
string(REPLACE "\n" ";" PALWORLD_FORMAT_FILES "${PALWORLD_FORMAT_FILES}")

set(PALWORLD_FORMAT_ABS_FILES "")
foreach(file_path IN LISTS PALWORLD_FORMAT_FILES)
    string(REPLACE "\\" "/" normalized_path "${file_path}")
    if(NOT normalized_path MATCHES "^mods/")
        continue()
    endif()
    list(APPEND PALWORLD_FORMAT_ABS_FILES "${SOURCE_DIR}/${normalized_path}")
endforeach()

if(NOT PALWORLD_FORMAT_ABS_FILES)
    message(STATUS "No tracked C/C++ files under mods/; nothing to format.")
    return()
endif()

if(MODE STREQUAL "fix")
    execute_process(
        COMMAND "${CLANG_FORMAT}" -i --style=file ${PALWORLD_FORMAT_ABS_FILES}
        COMMAND_ERROR_IS_FATAL ANY
    )
else()
    execute_process(
        COMMAND "${CLANG_FORMAT}" --dry-run --Werror --style=file
            ${PALWORLD_FORMAT_ABS_FILES}
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()
```

- [ ] **Step 3: 创建 format-only 版本的 `cmake/PalworldTooling.cmake`**

Write exactly:

```cmake
include_guard(GLOBAL)

find_package(Git QUIET)

find_program(PALWORLD_CLANG_FORMAT_EXECUTABLE
    NAMES clang-format-22 clang-format-21 clang-format-20 clang-format-19 clang-format-18
          clang-format
    DOC "clang-format binary used by the format targets"
)

if(PALWORLD_CLANG_FORMAT_EXECUTABLE
        AND GIT_FOUND
        AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    set(PALWORLD_RUN_CLANG_FORMAT
        "${CMAKE_SOURCE_DIR}/cmake/RunClangFormat.cmake")

    add_custom_target(format
        COMMAND ${CMAKE_COMMAND}
            "-DCLANG_FORMAT=${PALWORLD_CLANG_FORMAT_EXECUTABLE}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DMODE=fix"
            -P "${PALWORLD_RUN_CLANG_FORMAT}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Formatting tracked C/C++ files under mods/"
        VERBATIM
        USES_TERMINAL
    )

    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND}
            "-DCLANG_FORMAT=${PALWORLD_CLANG_FORMAT_EXECUTABLE}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DMODE=check"
            -P "${PALWORLD_RUN_CLANG_FORMAT}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking tracked C/C++ formatting under mods/"
        VERBATIM
        USES_TERMINAL
    )
else()
    message(STATUS
        "clang-format, Git, or a Git checkout is unavailable; "
        "format targets are disabled.")
endif()
```

- [ ] **Step 4: 从根 `CMakeLists.txt` 加载工具模块**

Immediately after `include(CTest)`, insert:

```cmake
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PalworldTooling)
```

不要增加 `CMAKE_CXX_CLANG_TIDY`，也不要给任何工具 target 添加 `ALL`。

- [ ] **Step 5: 重新配置并确认两个 target 已注册**

Run:

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target help |
    Select-String -Pattern '^format:|^format-check:'
```

Expected: 输出同时包含 `format` 和 `format-check`。

- [ ] **Step 6: 运行格式化并验证只修改 `mods/`**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format
cmake --build --preset ninja-msvc-x64 --target format-check
git diff --name-only -- RE-UE4SS
git diff --check
```

Expected:

- `format` 成功；
- `format-check` 成功且没有 replacements needed；
- `git diff --name-only -- RE-UE4SS` 无输出；
- `git diff --check` 无输出；
- C/C++ 格式变化只出现在上方列出的 `mods/MyPalMod/` 文件。

- [ ] **Step 7: 构建并运行测试，证明格式化未改变行为**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target MyPalMod MyPalModSkillTests
ctest --test-dir build --output-on-failure
```

Expected:

- `MyPalMod.dll` 构建成功；
- `MyPalModSkillTests` 构建成功；
- CTest 报告 `100% tests passed`。

- [ ] **Step 8: 提交 format 工具与格式化结果**

```powershell
git add CMakeLists.txt cmake/PalworldTooling.cmake cmake/RunClangFormat.cmake `
    mods/MyPalMod/src/dllmain.cpp `
    mods/MyPalMod/src/item_database.h `
    mods/MyPalMod/src/pal_game.hpp `
    mods/MyPalMod/src/pal_skills.cpp `
    mods/MyPalMod/src/pal_skills.hpp `
    mods/MyPalMod/src/skill_catalog.hpp `
    mods/MyPalMod/src/skill_editor_service.hpp `
    mods/MyPalMod/src/text_encoding.hpp `
    mods/MyPalMod/tests/skill_editor_tests.cpp
git commit -m "build: add scoped clang-format targets"
```

Expected: 不暂存 `.gitignore`、`AGENTS.md` 或 `UHTHeaderDump.7z`。

---

### Task 3: 增加 tidy 与 tidy-check target

**Files:**
- Modify: `cmake/PalworldTooling.cmake`
- Create: `cmake/RunClangTidy.cmake`

**Interfaces:**
- Consumes: `build/compile_commands.json`、根目录 `.clang-tidy`、Task 2 的工具模块。
- Produces: CMake target `tidy`、`tidy-check`；cache option `PALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS`；只接受 `mods/` 翻译单元的 runner。

- [ ] **Step 1: 证明 tidy target 当前不存在**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target tidy-check
```

Expected: 命令以 “unknown target 'tidy-check'” 或等价信息失败。

- [ ] **Step 2: 创建 `cmake/RunClangTidy.cmake`**

Write exactly:

```cmake
if(NOT CLANG_TIDY)
    message(FATAL_ERROR "RunClangTidy.cmake: CLANG_TIDY is not set")
endif()
if(NOT SOURCE_DIR)
    message(FATAL_ERROR "RunClangTidy.cmake: SOURCE_DIR is not set")
endif()
if(NOT BUILD_DIR)
    message(FATAL_ERROR "RunClangTidy.cmake: BUILD_DIR is not set")
endif()
if(NOT EXISTS "${BUILD_DIR}/compile_commands.json")
    message(FATAL_ERROR
        "RunClangTidy.cmake: ${BUILD_DIR}/compile_commands.json not found. "
        "Run cmake --preset ninja-msvc-x64 first.")
endif()
if(NOT MODE STREQUAL "fix" AND NOT MODE STREQUAL "check")
    message(FATAL_ERROR "RunClangTidy.cmake: MODE must be 'fix' or 'check'")
endif()

set(PALWORLD_TIDY_EXTRA_ARGS "")
if(MODE STREQUAL "fix")
    list(APPEND PALWORLD_TIDY_EXTRA_ARGS -fix)
endif()
if(WARNINGS_AS_ERRORS)
    list(APPEND PALWORLD_TIDY_EXTRA_ARGS -warnings-as-errors=*)
endif()

string(REPLACE "\\" "/" PALWORLD_SOURCE_DIR_NORMALIZED "${SOURCE_DIR}")
set(PALWORLD_SOURCE_FILTER "${PALWORLD_SOURCE_DIR_NORMALIZED}/mods/.*")

if(NOT WIN32)
    get_filename_component(CLANG_TIDY_DIR "${CLANG_TIDY}" DIRECTORY)
    get_filename_component(CLANG_TIDY_NAME "${CLANG_TIDY}" NAME)
    string(REGEX REPLACE "^clang-tidy" "run-clang-tidy"
        RUN_CLANG_TIDY_NAME "${CLANG_TIDY_NAME}")
    set(RUN_CLANG_TIDY_CANDIDATE
        "${CLANG_TIDY_DIR}/${RUN_CLANG_TIDY_NAME}")

    if(EXISTS "${RUN_CLANG_TIDY_CANDIDATE}")
        include(ProcessorCount)
        ProcessorCount(PALWORLD_TIDY_JOBS)
        if(PALWORLD_TIDY_JOBS EQUAL 0)
            set(PALWORLD_TIDY_JOBS 4)
        endif()

        execute_process(
            COMMAND "${RUN_CLANG_TIDY_CANDIDATE}"
                -p "${BUILD_DIR}"
                -clang-tidy-binary "${CLANG_TIDY}"
                -j ${PALWORLD_TIDY_JOBS}
                -quiet
                ${PALWORLD_TIDY_EXTRA_ARGS}
                "${PALWORLD_SOURCE_FILTER}"
            COMMAND_ERROR_IS_FATAL ANY
        )
        return()
    endif()
endif()

file(READ "${BUILD_DIR}/compile_commands.json" PALWORLD_COMPILE_COMMANDS)
string(JSON PALWORLD_COMPILE_COMMANDS_LENGTH
    ERROR_VARIABLE json_error
    LENGTH "${PALWORLD_COMPILE_COMMANDS}")
if(json_error)
    message(FATAL_ERROR
        "RunClangTidy.cmake: failed to parse compile_commands.json: ${json_error}")
endif()

set(PALWORLD_TIDY_FILES "")
if(PALWORLD_COMPILE_COMMANDS_LENGTH GREATER 0)
    math(EXPR last_index "${PALWORLD_COMPILE_COMMANDS_LENGTH} - 1")
    foreach(index RANGE 0 ${last_index})
        string(JSON compile_file GET "${PALWORLD_COMPILE_COMMANDS}" ${index} file)
        if(NOT IS_ABSOLUTE "${compile_file}")
            string(JSON compile_directory
                GET "${PALWORLD_COMPILE_COMMANDS}" ${index} directory)
            file(REAL_PATH "${compile_file}" compile_file
                BASE_DIRECTORY "${compile_directory}")
        endif()

        string(REPLACE "\\" "/" normalized_file "${compile_file}")
        if(normalized_file MATCHES
                "^${PALWORLD_SOURCE_DIR_NORMALIZED}/mods/.*\\.(cc|cpp|cxx)$")
            list(APPEND PALWORLD_TIDY_FILES "${compile_file}")
        endif()
    endforeach()
endif()

list(REMOVE_DUPLICATES PALWORLD_TIDY_FILES)

if(NOT PALWORLD_TIDY_FILES)
    message(STATUS
        "No translation units under mods/ were found in compile_commands.json.")
    return()
endif()

execute_process(
    COMMAND "${CLANG_TIDY}"
        -p "${BUILD_DIR}"
        ${PALWORLD_TIDY_EXTRA_ARGS}
        ${PALWORLD_TIDY_FILES}
    COMMAND_ERROR_IS_FATAL ANY
)
```

- [ ] **Step 3: 在 `cmake/PalworldTooling.cmake` 追加 tidy target 定义**

Append exactly:

```cmake
option(PALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS
    "Treat clang-tidy diagnostics as errors in tidy-check"
    OFF
)

get_property(PALWORLD_IS_MULTI_CONFIG
    GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

find_program(PALWORLD_CLANG_TIDY_EXECUTABLE
    NAMES clang-tidy-22 clang-tidy-21 clang-tidy-20 clang-tidy-19 clang-tidy-18
          clang-tidy
    DOC "clang-tidy binary used by the tidy targets"
)

if(PALWORLD_CLANG_TIDY_EXECUTABLE AND NOT PALWORLD_IS_MULTI_CONFIG)
    set(PALWORLD_RUN_CLANG_TIDY
        "${CMAKE_SOURCE_DIR}/cmake/RunClangTidy.cmake")

    add_custom_target(tidy
        COMMAND ${CMAKE_COMMAND}
            "-DCLANG_TIDY=${PALWORLD_CLANG_TIDY_EXECUTABLE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMODE=fix"
            "-DWARNINGS_AS_ERRORS=OFF"
            -P "${PALWORLD_RUN_CLANG_TIDY}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Running clang-tidy --fix over translation units under mods/"
        VERBATIM
        USES_TERMINAL
    )

    add_custom_target(tidy-check
        COMMAND ${CMAKE_COMMAND}
            "-DCLANG_TIDY=${PALWORLD_CLANG_TIDY_EXECUTABLE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMODE=check"
            "-DWARNINGS_AS_ERRORS=${PALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS}"
            -P "${PALWORLD_RUN_CLANG_TIDY}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Running clang-tidy over translation units under mods/"
        VERBATIM
        USES_TERMINAL
    )
else()
    message(STATUS
        "clang-tidy is unavailable or the generator is multi-config; "
        "tidy targets are disabled.")
endif()
```

- [ ] **Step 4: 重新配置并确认 target 与默认选项**

Run:

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target help |
    Select-String -Pattern '^tidy:|^tidy-check:'
cmake -LA -N build |
    Select-String -Pattern '^PALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS:BOOL=OFF$'
```

Expected:

- 输出包含 `tidy` 和 `tidy-check`；
- cache option 显示为 `OFF`。

- [ ] **Step 5: 只读运行 tidy-check**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target tidy-check
```

Expected:

- runner 从 `build/compile_commands.json` 读取翻译单元；
- 只检查 `mods/MyPalMod/src/*.cpp` 与 `mods/MyPalMod/tests/*.cpp`；
- 可以输出诊断，但默认不会因为普通 warning 被升级为 error；
- 不修改源文件。

- [ ] **Step 6: 验证缺失编译数据库时的明确错误**

Run:

```powershell
cmake -DCLANG_TIDY="$((Get-Command clang-tidy).Source)" `
    -DSOURCE_DIR="$PWD" `
    -DBUILD_DIR="$PWD/build/nonexistent-tidy-db" `
    -DMODE=check `
    -DWARNINGS_AS_ERRORS=OFF `
    -P cmake/RunClangTidy.cmake
```

Expected: 非零退出，并明确包含 `compile_commands.json not found` 和先运行 preset 的提示。

- [ ] **Step 7: 提交 tidy 工具**

```powershell
git add cmake/PalworldTooling.cmake cmake/RunClangTidy.cmake
git commit -m "build: add scoped clang-tidy targets"
```

Expected: commit 只包含两个 CMake 工具文件。

---

### Task 4: 记录用法并完成全量验证

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: Tasks 1–3 产生的四个 CMake target。
- Produces: 面向开发者的稳定命令说明；最终构建、测试、格式与静态检查证据。

- [ ] **Step 1: 在 README 快速开始之后增加代码质量说明**

Insert immediately after the “快速开始” command block:

````markdown
## 代码质量工具

clangd 会自动读取根目录的 `.clang-format`、`.clang-tidy` 和
`build/compile_commands.json`。CMake 还会在 PATH 中找到相应 LLVM 工具时注册以下手动 target；
它们不会随普通构建自动执行，也不会处理 `RE-UE4SS/`：

```powershell
# 按 Google 风格格式化 mods/ 下 Git 跟踪的 C/C++ 文件
cmake --build --preset ninja-msvc-x64 --target format

# 只检查格式，不修改文件
cmake --build --preset ninja-msvc-x64 --target format-check

# 对 compile_commands.json 中 mods/ 下的翻译单元应用 clang-tidy 修复
cmake --build --preset ninja-msvc-x64 --target tidy

# 只读静态检查；默认不会把 warning 升级为 error
cmake --build --preset ninja-msvc-x64 --target tidy-check
```

如需让 `tidy-check` 遇到任意诊断时失败，配置时增加
`-DPALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON`。修改 CMake 或新增源文件后，应重新运行
`cmake --preset ninja-msvc-x64` 刷新 `build/compile_commands.json`。
````

- [ ] **Step 2: 运行格式、配置、构建与测试验证**

Run:

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target format-check
cmake --build --preset ninja-msvc-x64 --target MyPalMod MyPalModSkillTests
ctest --test-dir build --output-on-failure
```

Expected:

- configure 成功；
- `format-check` 成功；
- 两个构建 target 成功；
- CTest 报告 `100% tests passed`。

- [ ] **Step 3: 运行静态检查与范围验证**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target tidy-check
git diff --check
git diff --name-only -- RE-UE4SS
git status --short
```

Expected:

- `tidy-check` 完成且不修改文件；
- `git diff --check` 无输出；
- `RE-UE4SS` 查询无输出；
- `git status` 仍保留用户原有 `.gitignore` 修改以及未跟踪的 `AGENTS.md`、
  `UHTHeaderDump.7z`，这些文件没有被暂存。

- [ ] **Step 4: 验证缺少 LLVM 工具不会令 CMake 配置失败**

Run:

```powershell
cmake -S . -B build/no-clang-tools -G Ninja `
    -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 `
    -DUE4SS_VERSION_CHECK=OFF `
    -DPALWORLD_CLANG_FORMAT_EXECUTABLE=OFF `
    -DPALWORLD_CLANG_TIDY_EXECUTABLE=OFF
cmake --build build/no-clang-tools --target help |
    Select-String -Pattern '^format:|^format-check:|^tidy:|^tidy-check:'
```

Expected:

- configure 成功；
- 输出说明 format/tidy target 已禁用；
- target help 查询无匹配，证明缺少可执行文件时只是跳过注册。

- [ ] **Step 5: 检查默认构建没有依赖工具 target**

Run:

```powershell
ninja -C build -t query MyPalMod |
    Select-String -Pattern 'format|tidy'
rg -n "add_custom_target\([^)]*ALL|CMAKE_CXX_CLANG_TIDY" `
    CMakeLists.txt cmake mods
```

Expected:

- 第一条命令没有 format/tidy 依赖；
- 第二条命令无命中。

- [ ] **Step 6: 提交 README**

```powershell
git add README.md
git commit -m "docs: document clang tooling targets"
```

Expected: commit 只包含 `README.md`。

- [ ] **Step 7: 最终提交范围复核**

Run:

```powershell
git log --oneline --max-count=5
git status --short
```

Expected:

- 最近提交包含配置、format 工具、tidy 工具与 README 四个逻辑提交；
- 工作树中仅剩用户原有 `.gitignore` 修改及两个未跟踪文件；
- 未创建部署文件，未修改 `RE-UE4SS/`。
