# 物品中文名展示设计

## 目标

让物品浏览器和背包列表像技能编辑器一样显示可读名称，格式统一为
`中文名 [RawId]`；带数量的背包条目显示为 `中文名 [RawId] ×数量`。搜索同时匹配本地化名称和原始 ID，
所有实际物品操作仍使用原始 ID。

名称通过 Palworld 运行时的 `UPalUIUtility::GetItemName()` 获取。该函数返回游戏当前语言的文本；
游戏语言为中文时显示中文名，不读取或解包本地化资源文件。

## 已确认的游戏接口

UHT 头信息提供以下函数：

```cpp
static void UPalUIUtility::GetItemName(
    const UObject* WorldContextObject,
    const FName& StaticItemId,
    FText& outName);
```

UE4SS 侧通过 `/Script/Pal.Default__PalUIUtility` 的类默认对象和
`/Script/Pal.PalUIUtility:GetItemName` UFunction 调用该接口。参数布局与技能名称查询采用相同模式：

```cpp
struct Params {
    UObject* WorldContextObject;
    FName StaticItemId;
    FText OutName;
};
```

`PalPlayerInventoryData` UObject 作为 world context。若该对象、本地化工具或 UFunction 尚不可用，
扫描仍保留物品 ID，只把本地化名称留空。

## 纯逻辑物品目录

新增 `mods/PalworldEditor/inc/items/item_catalog.hpp`，隔离不依赖 UE4SS 的目录逻辑：

```cpp
namespace item_catalog {

struct ItemOption {
    std::string id;
    std::string localizedName;
};

struct ItemCatalogSnapshot {
    std::vector<ItemOption> items;
    std::unordered_map<std::string, std::string> labelsById;
};

}
```

该模块提供以下行为：

- `item_label(const ItemOption&)`：名称存在时返回 `名称 [ID]`，否则返回 `ID`。
- `item_label(const ItemCatalogSnapshot&, std::string_view id)`：通过索引返回标签，未命中时返回 ID。
- `matches_item(const ItemOption&, std::string_view query)`：ASCII 不区分大小写匹配 ID 和名称；
  中文按 UTF-8 子串匹配。
- `filter_items(...)`：返回符合查询的目录项。
- `make_item_catalog(...)`：按 ID 去重，建立 `ID → 标签` 索引，并按显示标签排序。

目录结构中的 `items/` 是物品领域模块，不恢复此前删除的静态物品数据库。

## 运行时数据流

`pal_game::scan_all_items()` 继续在游戏线程遍历已加载的 `PalStaticItemData*` UObject。对每个有效对象：

1. 从 `ID` 属性读取原始物品 ID。
2. 调用 `PalUIUtility:GetItemName` 获取本地化 `FText`。
3. 转换成 UTF-8 `ItemOption`。
4. 扫描结束后由纯逻辑模块去重、排序并建立标签索引。

返回类型从 `std::vector<std::string>` 改为 `item_catalog::ItemCatalogSnapshot`。Mod 在现有
`inv_mutex_` 保护下整体替换缓存，因此 ImGui 线程只读取稳定的 C++ 字符串，不执行 Unreal 反射。

## UI 行为

### 物品浏览器

- 项数来自 `snapshot.items.size()`。
- 搜索同时匹配中文名和原始 ID。
- 每行显示 `中文名 [RawId]`。
- 点击条目只把 `RawId` 写入 Give 输入框。
- 未发现物品时沿用现有“尚未发现物品，请重新扫描”提示。

### 背包

- `InvEntry` 继续只保存物品 ID、数量和槽位，不复制本地化文本。
- 绘制列表时通过目录的 `labelsById` 查询标签，显示 `中文名 [RawId] ×数量`。
- 选中详情显示同一标签、槽位和数量。
- 目录尚未扫描、某个 ID 未收录或本地化名称为空时，标签回退为原始 ID。
- 修改数量仍按槽位执行，不受显示名称影响。

## 错误处理与兼容性

- `Default__PalUIUtility` 或 `GetItemName` 不存在：保留全部可扫描 ID，名称为空。
- world context 不存在：名称为空，后续用户重新扫描时可恢复。
- 单个名称查询返回空文本：仅该项退回 ID。
- 多个 UObject 产生相同 ID：以首次非空本地化名称为优先结果，最终目录只保留一个 ID。
- 游戏切换语言后：点击重新扫描即可刷新显示名称。
- 给予物品、背包修改和搜索 ID 的现有行为保持兼容。

## 测试与验证

在不链接 UE4SS 的 `PalworldEditorTests` 中覆盖：

1. `中文名 [ID]` 标签格式和空名称回退。
2. 中文名称搜索、大小写不敏感 ID 搜索和无匹配结果。
3. ID 去重以及“首次非空名称优先”规则。
4. `labelsById` 命中和未知 ID 回退。
5. 排序按最终显示标签稳定执行。

集成验证包括 `format-check`、`PalworldEditor` 构建、CTest、`tidy-check`，以及确认物品操作仍只提交原始 ID。
游戏内端到端验证需要在中文语言环境进入存档，检查物品浏览器和背包均显示中文名，并验证点击浏览器条目后
Give 输入框仍为 Raw ID。
