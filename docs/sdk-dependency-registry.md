# Palworld SDK 依赖清单（运行时反射核查用）

本清单登记 PalworldEditor mod 通过运行时反射按 `FName` 引用的全部 SDK 标识符，用于每次
Palworld 更新后快速核查兼容性。权威数据源是仓库内 `Dump/UHTHeaderDump/`（按类拆分的 UHT 头）
与 `Dump/CXXHeaderDump/*.hpp`（按模块合并的 CXX 头，含偏移）。

> **设计原则**：mod 遵循 `AGENTS.md` 的反射安全规则，**运行时按名精确校验、失败 fail-closed**，
> 从不依赖编译期偏移；一般结构按字段与属性子类校验，ABI 敏感结构还会校验运行时类型名与大小。
> 本清单登记「名称存在性」「值敏感项」和这些精确结构身份，不登记成员偏移。重命名或大小变化会让
> 对应功能安全停用（而非崩溃），但仍需核查以恢复功能。

## 核查流程

1. **重新生成主动技能表**：`pwsh scripts/generate-active-skill-definitions.ps1`，随后
   `git diff mods/PalworldEditor/inc/skills/active_skill_definitions.hpp`。若无新增/重编号技能，diff
   应仅含头注释（见下文「值敏感项」的 `EPalWazaID` 条目）。
2. **核对名称存在性**：按下表各节在 dump 中确认类、函数、字段名仍存在且签名匹配。重点核对带 ⚠️
   的「值敏感项」。
3. **记录结论**：在文末「核查历史」表追加一行（版本 / 日期 / 结论 / 破坏性变更摘要）。

dump 查找速查：
- 枚举：`Dump/UHTHeaderDump/Pal/Public/E<EnumName>.h`
- 类/结构体函数与字段：在 `Dump/CXXHeaderDump/Pal.hpp` 中 `grep -n "class U Pal<Name>" -A 400`
- 函数签名：`grep -n "<FunctionName>(" Dump/CXXHeaderDump/Pal.hpp`

---

## 一、UFunction 全路径（16 条）

这些通过 `/Script/Pal.<Class>:<Method>` 全路径解析（`StaticFindObject<UFunction*>`）。

| 路径 | 用途 | 源文件:行 |
|---|---|---|
| `PalIndividualCharacterParameter:ClearEquipWaza` | 清空装备主动技能 | `src/skills/pal_skills.cpp:249` |
| `PalIndividualCharacterParameter:AddEquipWaza` | 装备主动技能 | `src/skills/pal_skills.cpp:251` |
| `PalIndividualCharacterParameter:GetEquipWaza` | 读取已装备主动技能 | `src/skills/pal_skills.cpp:471` |
| `PalIndividualCharacterParameter:GetPassiveSkillList` | 读取已装备被动技能 | `src/skills/pal_skills.cpp:445` |
| `PalIndividualCharacterParameter:AddPassiveSkill` | 新增/替换被动技能 ⚠️ 双参数 | `src/skills/pal_skills.cpp:525` |
| `PalIndividualCharacterParameter:RemovePassiveSkill` | 删除被动技能 | `src/skills/pal_skills.cpp:555` |
| `PalPassiveSkillManager:GetSkillData` | 读取被动技能元数据 | `src/skills/pal_skills.cpp:710` |
| `PalPassiveSkillManager:GetPalAssignablePassiveIDs` | 读取可分配被动 ID 列表 | `src/skills/pal_skills.cpp:809` |
| `PalUIUtility:GetPassiveSkillName` | 被动技能本地化名称 | `src/skills/pal_skills.cpp:803` |
| `PalUIUtility:GetWazaName` | 主动技能本地化名称 | `src/skills/pal_skills.cpp:805` |
| `PalUtility:GetWazaDatabase` | 主动技能分类数据库 | `src/skills/pal_skills.cpp:851` |
| `PalPlayerInventoryData:TryGetContainerFromInventoryType` | 主背包容器 | `inc/game/pal_game.hpp:346` |
| `PalPlayerInventoryData:AddItem_ServerInternal` | 给予物品 | `inc/game/pal_game.hpp:512` |
| `PalUIUtility:GetItemName` | 物品本地化名称 | `inc/game/pal_game.hpp:712` |
| `PalUtility:GetItemIDManager` | 物品 ID 管理器 | `inc/game/pal_game.hpp:616` |
| `PalUtility:GetGameSetting` | 游戏设置单例 | `src/pal_stats/pal_stats.cpp:113` |

---

## 二、资源共享 Hook 清单（7 条）

定义于 `inc/base_resource_sharing/hook_manifest.hpp` 的 `kPalworld101HookManifest`。

| 路径 | 事件 | 必需 | 源文件:行 |
|---|---|---|---|
| `PalBaseCampModuleItemStorage:OnAvailableConcreteModel_ServerInternal` | structureChanged (pre) | 必需 | `:76-77` |
| `PalBaseCampModuleItemStorage:OnNotAvailableConcreteModel_ServerInternal` | structureChanged (pre) | 必需 | `:81-82` |
| `PalBaseCampModel:OnRep_ModuleArray` | structureChanged (post) | 必需 | `:85` |
| `PalUIBuildModel:OnOpenMenu` | ensurePersistentUnion (pre) | 可选 | `:88` |
| `PalNetworkPlayerComponent:RequestBuild_ToServer` | validatePersistentUnion (pre) | 必需 | `:91` |
| `PalUIConvertItemModel:Initialize` | ensurePersistentUnion (pre) | 可选 | `:93` |
| `PalUIConvertItemModel:StartProduction` | validatePersistentUnion (pre) | 必需 | `:95` |

## 二·B、捕获覆盖 Hook 清单（4 条）

定义于 `src/capture_override/capture_override_runtime.cpp`。仅在主开关开启时注册成对的
pre/post-hook；原生函数与 Blueprint 脚本函数统一通过 `common/function_hook_registry` 登记。
pre 回调完整校验签名并临时覆盖捕获字段，post 回调恢复精确原值。任一路径、签名、字段或恢复失败
则安全停用本世界。

| 路径 | 回调读取的参数 | 源文件:行 |
|---|---|---|
| `PalSphereBodyBase:SetupInternal` | `TargetCharacter`（唯一参数） | `capture_override_runtime.cpp` `kCaptureHookManifest` |
| `PalPlayerController:SetupInternalForSphere` | `TargetCharacter`（末参） | 同上 |
| `PalPlayerController:SetupInternalForSphere_ToServer` | `TargetCharacter`（末参） | 同上 |
| `PalPlayerController:SetupInternalForSphere_ToALL` | `TargetCharacter`（末参） | 同上 |

pre 回调临时写入、post 回调恢复的字段按开关分组（两开关相互独立，任一启用即登记 Hook）：

- **解锁开关**：`StaticCharacterParameterComponent` 的 `IsUncapturable`、`IsBoss_Database`、
  `IsTowerBoss_Database`、`IsRaidBoss_Database`、`IsPredatorBoss_Database`、`IsRaidBoss_BP`
  （均 bool=false）与 `IsPal`（bool=true；对人类 NPC 是独立捕获门控，对真帕鲁为无变化空操作）；
  个体参数 `bIsUncapturable`（bool=false，经 `SetUncapturable(bool)` 原生 setter 通知）。
- **强制开关**：`CaptureSuccessRate`（float=9999.0）、`SetSpawnedCharacterType(0)`、
  `bIsForceCapturable`（bool=true，经 `SetForceCapturable(bool)` 原生 setter 通知）。
  仅强制模式会先读 `IsPal` 并跳过非帕鲁目标（资格门控属解锁职责，写概率字段无意义）。

角色→组件路径：`CharacterParameterComponent` 字段 → `GetIndividualParameter()` UFunction。

---

## 三、CDO / UClass / BP 资产路径（8 条）

| 路径 | 用途 | 源文件:行 |
|---|---|---|
| `/Script/Pal.PalIndividualCharacterParameter` | UClass IsChildOf 校验 | `inc/game/pal_game.hpp:265` |
| `/Script/Pal.PalBaseCampModuleItemStorage` | 存储模块类筛选 | `src/base_resource_sharing/pal_base_resource_runtime.cpp:534` |
| `/Script/Pal.PalStaticItemDataBase` | 堆叠上限覆盖目标 | `src/items/stack_limit_gateway.cpp:24` |
| `/Script/Pal.PalHUDDispatchParameter_PalBox` | 终端派发参数类 | `src/pal_remote_palbox/remote_palbox_runtime.cpp:716` |
| `/Script/Pal.Default__PalUIUtility` | UI 工具 CDO | `inc/game/pal_game.hpp:561`, `src/skills/pal_skills.cpp:73` |
| `/Script/Pal.Default__PalUtility` | 工具 CDO（多处） | `inc/game/pal_game.hpp:614` 等 |
| `/Game/Pal/Blueprint/UI/PalStorage/WBP_PalStorageMenu` | 终端 widget 软引用 | `src/pal_remote_palbox/remote_palbox_runtime.cpp:58-59` |
| `/Game/.../WBP_PalStorageMenu.WBP_PalStorageMenu_C` | 终端 widget 生成类 | `src/pal_remote_palbox/remote_palbox_runtime.cpp:60-61` |

FindFirstOf / FindAllOf 短类名：`PalPlayerInventoryData`、`PalOtomoHolderComponentBase`、
`PalUIUtility`、`PalPassiveSkillManager`、`PalDatabaseCharacterParameter`、`PalStaticItemDataBase`、
`PalWeaponBase`、`PalPlayerState`、`PalBaseCampManager`、`PalMapObjectManager`、`PalHUDService`。

---

## 四、按对象解析的函数名（约 40 条）

通过 `GetFunctionByNameInChain(STR("<Name>"))` 在对象类链中解析。

### 技能 / 个体参数（`UPalIndividualCharacterParameter`、Handle、数据库）

| 函数名 | 所属类 | 用途 | 源文件:行 |
|---|---|---|---|
| `GetOtomoIndividualHandle` | `UPalOtomoHolderComponentBase` | 按槽取 Otomo Handle | `inc/game/pal_game.hpp:125` |
| `TryGetSpawnedOtomoHandle` | `UPalOtomoHolderComponentBase` | 当前出战判断（形态锁定） | `inc/game/pal_game.hpp:231` |
| `TryGetIndividualParameter` | Handle | 取个体参数对象 | `inc/game/pal_game.hpp:247`, `src/pal_revive/pal_revive.cpp:210` |
| `GetIndividualParameter` | CharacterParameterComponent | 捕获覆盖目标个体参数 | `src/capture_override/capture_override_runtime.cpp` |
| `GetPalId` | Handle | 取 `FPalInstanceID` | `inc/game/pal_game.hpp:270` |
| `GetCharacterID` | Parameter | 读 CharacterID | `inc/game/pal_game.hpp:294`, `src/pal_identity/pal_identity.cpp:226` |
| `GetSelectedOtomoID` | Holder | 当前选中 Otomo | `inc/game/pal_game.hpp:214` |
| `OnRep_SaveParameter` | `UPalIndividualCharacterParameter` | 写后刷新 | `src/pal_stats/pal_stats.cpp:605` 等 |
| `GetWorkSuitabilityRankWithCharacterRank` | Parameter | 工作适应性基础等级 | `src/pal_stats/pal_stats.cpp:213, 222` |
| `SetWorkSuitabilityAddRank` | Parameter | 工作适应性增量写入 | `src/pal_stats/pal_stats.cpp:310` |
| `UpdateApplyDatabaseToIndividualParameter` | `UPalDatabaseCharacterParameter` | 数据库刷新 | `src/pal_stats/pal_stats.cpp:371` |
| `GetFriendshipRequiredPointByRank` | `UPalDatabaseCharacterParameter` | 亲密度阈值 | `src/pal_stats/pal_stats.cpp:395` |
| `IsRarePal` | Parameter | Lucky 读取 | `src/pal_identity/pal_identity.cpp:227` |
| `IsAwakening` | Parameter | 觉醒读取 | `src/pal_identity/pal_identity.cpp:228` |
| `GetDatabaseCharacterParameter` | `UPalUtility` | 数据库单例 | `src/pal_identity/pal_identity.cpp:78` |
| `GetIsBoss`/`GetIsTowerBoss`/`GetIsRaidBoss`/`GetIsPredatorBoss` | `UPalDatabaseCharacterParameter` | Boss 分类 | `src/pal_identity/pal_identity.cpp:185, 161-163` |
| `SetPhysicalHealth` | Parameter | 复活写入 | `src/pal_revive/pal_revive.cpp:72` |
| `FullRecoveryHP` | Parameter | 满血恢复 | `src/pal_revive/pal_revive.cpp:80` |
| `SetUncapturable`/`SetForceCapturable` | Parameter | 临时覆盖并恢复个体捕获标志 | `src/capture_override/capture_override_runtime.cpp` |
| `SetSpawnedCharacterType` | StaticCharacterParameterComponent | 强制模式临时切换 Common 类型 | `src/capture_override/capture_override_runtime.cpp` |
| `GetMaxOtomoNum` | Holder | 队伍上限 | `src/pal_revive/pal_revive.cpp:193` |
| `GetLocationManager` | `UPalUtility` | 标记传送：位置管理器 | `src/waypoint_teleport/waypoint_teleport_runtime.cpp` |
| `SetNoFallDamageHeightLastJumpedLocation` | `UPalIndividualCharacterParameter` | 标记传送：每次放置后重置坠落伤害下落起点（游戏原生机制，`LastJumpedLocation` 与落点差值结算） | `src/waypoint_teleport/waypoint_teleport_runtime.cpp` |
| `LineTraceSingle` | `UKismetSystemLibrary` | 标记传送：地面高度追踪（通道 0，±1km 窗口，读 OutHit.ImpactPoint.Z；PalSquadAllOut 同款） | `src/waypoint_teleport/waypoint_teleport_runtime.cpp` |
| `K2_SetActorLocation` | `AActor`（玩家 Pawn） | 标记传送：无扫掠精确放置（bSweep=false, bTeleport=true）。`SyncTeleport`（有状态序列，EngineTick 前置相位下内部 -1 崩溃）与 `K2_TeleportTo`（路径扫掠，直线穿山时在阻挡点停下导致入地）均已弃用 | 同上 |
| `FindWazaForBP` | `UPalWazaDatabase` | 主动技能分类查询 | `src/skills/pal_skills.cpp:855` |

### 玩家 / 控制器 / HUD

| 函数名 | 所属类 | 用途 | 源文件:行 |
|---|---|---|---|
| `TryGetOwnerControlledPawn` | Pawn | 取控制器 | `inc/game/pal_game.hpp:72` |
| `GetController` | Pawn | 控制器 | `inc/game/pal_game.hpp:75` |
| `IsLocalPlayerController` | Controller | 本地判断 | `inc/game/pal_game.hpp:78` |
| `K2_GetPawn`/`GetPawn` | Controller | 取 Pawn | `inc/base_resource_sharing/current_base_resolution.hpp:22, 30` |
| `GetInsideBaseCampModel` | `InsideBaseCampCheckComponent` | 所在据点 | `inc/base_resource_sharing/current_base_resolution.hpp:24, 32` |
| `K2_GetActorLocation`/`GetActorLocation` | Actor | 位置 | `src/pal_remote_palbox/remote_palbox_runtime.cpp:176` |
| `GetTransform` | BaseCampModel | 据点变换 | `src/pal_remote_palbox/remote_palbox_runtime.cpp:262` |
| `GetVisibility`/`IsInViewport` | Widget | 可见性 | `src/pal_remote_palbox/remote_palbox_runtime.cpp:334, 325` |
| `GetHUD` | Controller | HUD | `src/pal_remote_palbox/remote_palbox_runtime.cpp:366` |
| `CreateDispatchParameterForK2Node`/`Push` | HUD | 终端派发 | `src/pal_remote_palbox/remote_palbox_runtime.cpp:705, 707` |
| `IsInStage`/`IsRiding` | PlayerCharacter | 场景判断 | `src/pal_remote_palbox/remote_palbox_runtime.cpp:577, 581` |
| `GetOwnerMapObjectInstanceId` | ConcreteModel | 容器归属 | `src/pal_remote_palbox/remote_palbox_runtime.cpp:626` |

### 据点 / 地图对象 / 资源共享

| 函数名 | 所属类 | 用途 | 源文件:行 |
|---|---|---|---|
| `IsServer`/`IsDedicatedServer` | World | 权限判断 | `src/base_resource_sharing/pal_base_resource_runtime.cpp:445, 446` |
| `GetLocalPalPlayerController` | `UPalUtility` | 本地控制器 | `pal_base_resource_runtime.cpp:176, 474`, `remote_palbox_runtime.cpp:74` |
| `GetPlayerUId` | PlayerState | 玩家 GUID | `pal_base_resource_runtime.cpp:182` |
| `GetGuildByPlayerUId` | GuildManager | 公会查找 | `pal_base_resource_runtime.cpp:148` |
| `GetId` | BaseCampModel/Guild/ConcreteModel | GUID 读取 | `pal_base_resource_runtime.cpp:184, 270, 340` |
| `GetBaseCampManager`/`GetMapObjectManager`/`GetLocalInventoryData`/`GetGameSetting` | `UPalUtility` | 管理器单例 | `pal_base_resource_runtime.cpp:526, 527, 751` |
| `GetGroupIdBelongTo` | BaseCampModel | 据点公会 | `pal_base_resource_runtime.cpp:549` |
| `GetItemContainerModule`/`GetContainer` | ConcreteModel | 容器解析 | `pal_base_resource_runtime.cpp:338, 339` |
| `OnRep_ContainerInfos`/`OnRep_Containers` | Storage/Inventory | 数组变更通知 | `pal_base_resource_runtime.cpp:418, 733, 790, 893` |
| `OnAvailableConcreteModel_ServerInternal`/`OnNotAvailableConcreteModel_ServerInternal` | ItemStorage | 容器事件 | `pal_base_resource_runtime.cpp:930, 938, 948, 994` |
| `OnUpdateInventory` | BuildModel | 建造库存 | `pal_base_resource_runtime.cpp:1064` |
| `GetBaseCampIds`/`TryGetModel` | `UPalBaseCampManager` | 据点枚举 | `src/game/pal_base_camp_reflection.cpp:20, 49` |
| `FindConcreteModel` | `UPalMapObjectManager` | ConcreteModel 解析 | `pal_base_camp_reflection.cpp:84`, `remote_palbox_runtime.cpp:799, 800` |
| `Num`/`Get` | ItemContainer | 槽位数量/取值 | `inc/game/pal_game.hpp:381, 396` |

---

## 五、SaveParameter 字段（`FPalIndividualCharacterSaveParameter`）

通过 `SaveParameter` 的 `FStructProperty` 入口 + `GetPropertyByNameInChain` 按名访问。

| 字段 | 类型 | 用途 | 源文件:行 |
|---|---|---|---|
| `SaveParameter` | struct（入口） | 参数根 | `src/pal_stats/pal_stats.cpp:37` 等 |
| `MasteredWaza` | `TArray<EPalWazaID>` | 已掌握主动技能 | `src/skills/pal_skills.cpp:305` |
| `Talent_HP`/`Talent_Shot`/`Talent_Defense` | uint8 | 个体值 | `src/pal_stats/pal_stats.cpp:438-440` |
| `Rank_HP`/`Rank_Attack`/`Rank_Defence`/`Rank_CraftSpeed` | uint8 | 帕鲁之魂强化 | `src/pal_stats/pal_stats.cpp:441-444` |
| `Rank` | uint8 | 浓缩内部星级 | `src/pal_stats/pal_stats.cpp:445` |
| `RankUpExp` | uint16 | 浓缩经验 | `src/pal_stats/pal_stats.cpp:446` |
| `Level` | uint8 | 等级 | `src/pal_stats/pal_stats.cpp:568` |
| `Gender` | enum（`EPalGenderType`） | 性别 ⚠️ 值敏感 | `src/pal_stats/pal_stats.cpp:447` |
| `CharacterID` | FName | 物种 ID | `src/pal_stats/pal_stats.cpp:467`, `src/pal_identity/pal_identity.cpp:51` |
| `GotWorkSuitabilityAddRankList` | `TArray<FPalWorkSuitabilityInfo>` | 工作适应性永久加成 | `src/pal_stats/pal_stats.cpp:158` |
| `FriendshipPoint` | int32 | 亲密度 | `src/pal_stats/pal_stats.cpp:596` |
| `IsRarePal` | bool | Lucky | `src/pal_identity/pal_identity.cpp:54` |
| `bIsAwakening` | bool | 觉醒 | `src/pal_identity/pal_identity.cpp:58` |
| `PhysicalHealth` | enum（`EPalStatusPhysicalHealthType`） | 生命状态 | `src/pal_revive/pal_revive.cpp:55` |
| `Hp` | struct（`FFixedPoint64`） | HP 容器 | `src/pal_revive/pal_revive.cpp:62` |

`GotWorkSuitabilityAddRankList` 元素结构 `FPalWorkSuitabilityInfo` 子字段：`WorkSuitability`(enum) +
`Rank`(int32)（`src/pal_stats/pal_stats.cpp:162-166`）。`Hp` 子字段：`Value`(int64)。

---

## 六、其他结构体 / 对象字段

| 字段 | 容器 | 类型 | 源文件:行 |
|---|---|---|---|
| `StackCount` | ItemSlot | int | `inc/game/pal_game.hpp:427, 488` |
| `ItemId` | ItemSlot | `FPalItemId` 结构（首成员 `StaticId` FName） | `inc/game/pal_game.hpp:477` |
| `ID` | StaticItemData/Container | FName | `inc/game/pal_game.hpp:754` 等 |
| `MaxStackCount` | `PalStaticItemDataBase` | int32 | `src/items/stack_limit_gateway.cpp:93` |
| `StaticItemDataAsset` | `PalItemIDManager` | object | `inc/game/pal_game.hpp:639` |
| `StaticItemDataMap` | `PalStaticItemDataAsset` | `TMap<FName, PalStaticItemDataBase*>` | `inc/game/pal_game.hpp:658` |
| `ownItemID` | `PalWeaponBase` | struct | `src/grappling_hook/grapple_cooldown_gateway.cpp:43` |
| `StaticId` | 武器 item id | FName | `grapple_cooldown_gateway.cpp:52` |
| `CoolDownTime` | `PalWeaponBase` | float | `grapple_cooldown_gateway.cpp:97` |
| `ModuleArray` | `PalBaseCampModel` | `TArray<ModuleBase*>` | `pal_base_resource_runtime.cpp:558` |
| `AreaRange` | `PalBaseCampModel` | float（据点模型范围，不用） | `remote_palbox_runtime.cpp:163` |
| `BaseCampAreaRange` | GameSetting | float（视觉建造圈，用） | `remote_palbox_runtime.cpp:150` |
| `ContainerInfos` | `PalBaseCampModuleItemStorage` | `TArray<ContainerInfo>` | `pal_base_resource_runtime.cpp:367` 等 |
| `Containers` | InventoryMultiHelper | array | `pal_base_resource_runtime.cpp:403` 等 |
| `OwnerMapObjectConcreteModelInstanceId` | `FPalBaseCampItemContainerInfo` | FGuid | `pal_base_resource_runtime.cpp:208` |
| `ContainerIdCache` | 同上 | struct（含 `ID`） | `pal_base_resource_runtime.cpp:212` |
| `Type` | 同上 | byte（0=Chest） | `pal_base_resource_runtime.cpp:220` |
| `InstanceId` | `FPalInstanceID`/ConcreteModel | FGuid | `pal_base_resources.cpp:555` 等 |
| `ConcreteModel` | Hook 参数 | object | `pal_base_resource_runtime.cpp:868` |
| `InsideBaseCampCheckComponent` | PlayerCharacter | object | `current_base_resolution.hpp:23` |
| `Translation`/`Transform` | FTransform | struct | `remote_palbox_runtime.cpp:252, 274` |
| `PalBoxWiget` | DispatchParameter | FClassProperty（游戏原拼写） | `remote_palbox_runtime.cpp:289` |
| `Pawn` | Controller | object | `remote_palbox_runtime.cpp:204` |
| `bIsBattleMode` | `APalCharacter` | bool（战斗判定） | `remote_palbox_runtime.cpp:234` |
| `MyHUD` | Controller | object | `remote_palbox_runtime.cpp:370` |
| `StackableUIWidgets` | `APalHUDInGame` | array | `remote_palbox_runtime.cpp:383` |
| `bShowMouseCursor` | PlayerController | bool | `remote_palbox_runtime.cpp:422` |
| `bIsCompleteSyncPlayerFromServer_InClient` | PlayerState | bool | `remote_palbox_runtime.cpp:570` |
| `BaseCampId`/`OwnerMapObjectInstanceId` | `PalHUDDispatchParameter_PalBox` | struct | `remote_palbox_runtime.cpp:747, 749` |
| `CharacterMaxRank`/`WorkSuitabilityMaxRank` | `PalGameSetting` | int | `src/pal_stats/pal_stats.cpp:134, 136` |
| `PalBoxReviveTime` | `PalGameSetting` | float（可逆清零，恢复账本） | `src/revive_timer/revive_timer_gateway.cpp` |
| `CustomMarkers` | `UPalLocationManager` | `TMap<FGuid, FPalCustomMarkerSaveData>`（只读迭代，读取值结构 `IconLocation` FVector） | `src/waypoint_teleport/waypoint_teleport_runtime.cpp` |

---

## 七、值敏感项（⚠️ 核查重点）

这些项的**具体数值/参数数量**被项目硬编码或范围校验，必须在 dump 中确认未变。

| 标识符 | 项目期望 | dump 核对要点 |
|---|---|---|
| `EPalWazaID` | None=0，技能 1–390，MAX=391（当前 390 条） | 重新生成表后 `git diff`；确认无重编号/插入/删除 |
| `EPalWorkSuitability` | 仅用值 1–13（emitFlame…monsterFarm） | dump 另有 None=0/Anyone=14/MAX=15，项目越界即拒绝（`pal_stats.cpp:181-183`）；确认 1–13 顺序不变 |
| `EPalWazaCategory` | Melee=0/Shot=1/Support=2 | 确认三值不变（`skill_catalog.hpp:77-81`） |
| `EPalGenderType` | none=0/male=1/female=2 | 确认三值不变（`pal_stat_editor.hpp:23-27`） |
| `FPalInstanceID.InstanceId` | `FGuid`（16 字节） | 确认仍是 FGuid（`pal_game.hpp:279` 校验 `sizeof(FGuid)`） |
| `AddPassiveSkill` | 2 参数 `(AddSkill, OverrideSkill)` | 确认仍为 2 参（`pal_skills.cpp:534` 校验 `has_exact_parameter_count(2)`） |
| `EPalBaseCampItemContainerType` | Chest=0 | 确认 Chest 仍为 0（`pal_base_resource_runtime.cpp:615` 用 `!= 0` 判非箱） |
| 容器事件 Hook 参数 | `UPalMapObjectConcreteModelBase*` | 确认两 Hook 仍接收 ConcreteModel 指针 |
| `FPalContainerId` | 类型名 `PalContainerId`、16 字节，内部 `ID` 为 `FGuid` | 容器持久 ID 读取依赖包装结构身份与大小 |
| `FVector` / `FVector_NetQuantize` | 类型名 `Vector` / `Vector_NetQuantize`、均为 0x18 字节 | 传送输入只接受 `Vector`；追踪命中点允许这两个已确认变体 |
| `FHitResult` | 类型名 `HitResult`、0xE8 字节 | `LineTraceSingle.OutHit` 与 `K2_SetActorLocation.SweepHitResult` 均精确校验 |
| `FLinearColor` | 类型名 `LinearColor`、0x10 字节 | `LineTraceSingle.TraceColor` / `TraceHitColor` 精确校验 |

> **注**：未列为 ABI 敏感项的领域结构通常只按字段名、子字段名和具体 `FProperty` 子类校验；上表所列
> 结构以及 `FGuid`、`FTransform` 等直接复制或解释的结构必须同时匹配运行时类型名与大小。

---

## 八、已知版本标记（代码中写死的版本字符串）

更新目标 Palworld 版本时，应一并审视这些处是否需同步：

| 标记 | 位置 | 说明 |
|---|---|---|
| `kPalworld101HookManifest` | `inc/base_resource_sharing/hook_manifest.hpp:73` | Hook 清单符号名；历史命名，改动会扩大 diff |
| `palworld_1_0_1_hook_manifest()` | 同上 `:98` | 访问函数名；同上 |
| `"Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。"` | 同上 `:201` | 修理不可用提示文案 |
| `"Palworld 1.0.2 可编辑的具体工作适应性值..."` | `inc/pal_stats/pal_stat_editor.hpp:29` | 枚举注释 |
| `"Palworld 1.0.2 的 GetWorkSuitabilityRank..."` | `docs/pal-individual-field-audit.md:24` | 审计文档说明 |
| `PalworldEditor 1.7.0` / `Palworld 1.0` | `README.md:1, 3` | README 标题与描述 |
| `PalworldEditor loaded (v1.7.0)` | `AGENTS.md` 验证清单 | 控制台预期输出 |

---

## 九、核查历史

| Palworld 版本 | 核查日期 | 结论 | 破坏性变更 | 核查者 |
|---|---|---|---|---|
| 1.0.3 | 2026-08-14 | ✅ 完全兼容，无需改动 | 无；所有反射标识符存在且签名/数值一致 | ZCode |

> 新增核查记录时复制最后一行并更新。若发现破坏性变更，在「破坏性变更」列简述受影响标识符与修复方式，
> 并在相关 `src/`/`inc/` 代码处补注释说明版本差异处理。
