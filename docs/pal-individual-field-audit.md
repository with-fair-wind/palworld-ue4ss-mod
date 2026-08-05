# 帕鲁个体字段可编辑性审计（Palworld 1.0.2）

本审计以 `Dump/UHTHeaderDump/Pal/Public/PalIndividualCharacterSaveParameter.h`、
`PalIndividualCharacterParameter.h` 和相关枚举/设置类型为准。结论面向运行中 UE4SS Mod：所有反射访问只能在
游戏线程发生，跨帧只保存纯值，不缓存 UObject 或属性地址。

## 已实现且具有明确事务边界

| 属性 | 权威数据/接口 | 处理方式 |
|---|---|---|
| 等级 | `Level` | 限制到 1–80，差量写入、重读、失败回滚 |
| 个体值 | `Talent_HP` / `Talent_Shot` / `Talent_Defense` | 限制到 0–100 |
| 帕鲁之魂强化 | `Rank_HP` / `Rank_Attack` / `Rank_Defence` / `Rank_CraftSpeed` | 各限制到 0–20，不调用材料消耗流程；写后调用 `OnRep_SaveParameter` |
| 浓缩星级 | `Rank` / `RankUpExp` | UI 星级 = Rank - 1；上限读取 `CharacterMaxRank`，直接设星时清零未完成浓缩经验 |
| 性别 | `Gender` | 只允许 `Male` / `Female` |
| 工作适应性永久加成 | `GotWorkSuitabilityAddRankList` + `SetWorkSuitabilityAddRank` | UI 直接编辑存档中的绝对永久附加值；游戏线程适配器计算相对当前值的有符号差值后调用原生增量 setter；基础值与附加值之和不得超过 `WorkSuitabilityMaxRank` |
| 亲密度 | `FriendshipPoint` + `GetFriendshipRequiredPointByRank` | 由目标 rank 换算为游戏阈值 |
| Alpha / 头目 | 普通与 `BOSS_` CharacterID 配对 + 数据库 Boss 分类 | 只允许原生数据库确认的普通 Alpha 配对；拒绝塔主、团本和捕食者 Boss |
| Lucky / 闪光 | `IsRarePal` | 与 Alpha 独立；写入后调用原生刷新并重读验证 |
| 觉醒 | `bIsAwakening` | 与 Alpha/Lucky 独立；写入后调用原生刷新并重读验证 |
| 主动/被动技能 | `EquipWaza` / `PassiveSkillList` 等原有路径 | 由独立技能事务服务处理 |

浓缩 Rank 同时决定伙伴技能等级，因此只显示派生等级，不再单独写一个并不存在的“伙伴技能等级”个体字段。
Palworld 1.0.2 的 `GetWorkSuitabilityRank` 返回值不包含 `GotWorkSuitabilityAddRankList`，因此只作为基础等级；
面板合计等级由“基础等级 + 永久附加值”计算。物种本身不具备的方向保持只读，不得通过伪造附加值创建新适应性，
也不得写 `CraftSpeed` / `CraftSpeeds` 等瞬时派生缓存。其事务失败只停用工作适应性写入域。

## 可继续扩展，但应优先走原生接口

| 候选属性 | 相关字段/接口 | 风险与建议 |
|---|---|---|
| 昵称 | `NickName` 与昵称更新委托 | UHT 未暴露直接 setter；需先定位原版改名/网络入口，不能只写 FString |
| 收藏/最爱 | `IsFavoritePal` 与收藏索引/亲密度委托 | UHT 未暴露直接 setter；需先定位盒子 UI 的原生切换入口 |
| 外观皮肤 | `SkinName`、`SkinAppliedCharacterId` 及 setter | 可做；必须校验皮肤与物种兼容，不能接受任意 Raw ID |
| 语音 ID | `VoiceId` | UHT 未暴露直接 setter；需从角色静态数据建立合法目录并识别刷新路径 |
| 当前 HP、饥饿、SAN | 对应 SaveParameter 与运行时 setter | 可做即时恢复类按钮；不宜作为任意永久数值编辑，需同步状态组件/上限 |
| 疾病、受伤、工作偏好 | 状态字段与原生 setter | 可做；应使用枚举目录和原生状态迁移，不能只改一个字节 |
| 未用属性点/已领取点数 | `UnusedStatusPoint`、`GotStatusPointList`、`GotExStatusPointList` | 可做但属于多字段不变量，必须先复现游戏加点/返还规则 |

## 不应作为普通个体编辑项

- 任意 `CharacterID` / `UniqueNPCID` / `OriginalCharacterID`：决定物种或 NPC 身份，禁止自由输入；唯一例外是
  通过原生数据库验证的同物种普通/`BOSS_` Alpha 配对事务。
- 元素：来自角色数据库 `FPalCharacterParameterDatabaseRow.ElementType1/2`，不是独立的个体存档字段。
- 伙伴技能类型：由物种/CharacterID 定义；只有伙伴技能等级随浓缩 Rank 改变。
- `MaxHP`、`CraftSpeed`、`CraftSpeeds`、战斗数值：属于瞬时派生缓存，直接写会被重算或造成显示/存档不一致。
- Container/Slot/Owner/Guild/Individual GUID：属于所有权和存储拓扑，不是玩法属性；误写可导致丢帕鲁或坏档。
- `WorkSuitabilityOverflowGrantedRankList`：游戏维护的溢出补偿账本，不能与永久加成数组分开编辑。
- 竞技场、远征、冷却时间戳、迁移版本等流程状态：由对应系统拥有，普通属性面板不应修改。

## 后续实现优先级

1. 合法皮肤选择：已有原生 setter，但必须先从当前物种静态数据构建白名单。
2. HP/饥饿/SAN 的“恢复到上限”操作：使用原生接口，不开放任意超范围值。
3. 昵称与收藏/最爱：先定位原版修改入口和网络权限路径，再接入事务。
4. 疾病/受伤清除：建立枚举目录并测试状态组件刷新。
5. 属性点：只有在完整识别所有联动字段并覆盖回滚测试后再实现。
