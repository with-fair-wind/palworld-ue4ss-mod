/**
 * @file pal_stats.cpp
 * @brief 实现帕鲁属性编辑网关：导航 `SaveParameter` 并读写等级、个体值、强化与亲密度。
 * @details 所有接口在游戏线程执行，所有 Unreal 裸指针均为非拥有观察指针，
 *          不跨调用缓存任何句柄或属性指针。
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <game/pal_game.hpp>
#include <pal_stats/pal_stat_editor.hpp>
#include <pal_stats/pal_stats.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace pal_stats {
using pal_game::FunctionParams;
namespace {
/** @brief 把目标句柄还原为非拥有帕鲁 UObject，失效时返回 `nullptr`。 */
[[nodiscard]] auto to_pal(const PalStatTarget target) -> UObject* {
    auto* pal = reinterpret_cast<UObject*>(target);
    return pal_game::is_valid(pal) ? pal : nullptr;
}

/** @brief 取得帕鲁 `SaveParameter` 结构体内存指针与其 `FStructProperty`；不可达时返回 `nullptr`。
 */
[[nodiscard]] auto save_parameter_slot(UObject* pal, FStructProperty*& outProperty) -> void* {
    auto* const prop = pal->GetPropertyByNameInChain(STR("SaveParameter"));
    outProperty = CastField<FStructProperty>(prop);
    if (outProperty == nullptr || outProperty->GetStruct().Get() == nullptr) {
        return nullptr;
    }
    return outProperty->ContainerPtrToValuePtr<void>(pal);
}

/** @brief 查找 `SaveParameter` 中一个 `uint8` 字段；缺失或类型不匹配时返回空。 */
[[nodiscard]] auto find_byte(UStruct* rowStruct, const TCHAR* name) -> FByteProperty* {
    return rowStruct == nullptr
               ? nullptr
               : CastField<FByteProperty>(rowStruct->FindProperty(FName(name, FNAME_Find)));
}

/** @brief 查找 `uint16` 属性；字段布局不匹配时返回空。 */
[[nodiscard]] auto find_uint16(UStruct* rowStruct, const TCHAR* name) -> FUInt16Property* {
    return rowStruct == nullptr
               ? nullptr
               : CastField<FUInt16Property>(rowStruct->FindProperty(FName(name, FNAME_Find)));
}

/** @brief 查找底层为整数的枚举属性；不接受未知的非整数布局。 */
[[nodiscard]] auto find_enum(UStruct* ownerStruct, const TCHAR* name) -> FEnumProperty* {
    auto* const property =
        ownerStruct == nullptr
            ? nullptr
            : CastField<FEnumProperty>(ownerStruct->FindProperty(FName(name, FNAME_Find)));
    auto* const underlying = property == nullptr ? nullptr : property->GetUnderlyingProperty();
    return underlying != nullptr && underlying->IsInteger() ? property : nullptr;
}

/** @brief 从已预检的枚举属性读取底层整数。 */
[[nodiscard]] auto read_enum(FEnumProperty* property, const void* container) -> std::optional<int> {
    auto* const underlying = property == nullptr ? nullptr : property->GetUnderlyingProperty();
    auto* const data = property == nullptr
                           ? nullptr
                           : property->ContainerPtrToValuePtr<void>(const_cast<void*>(container));
    if (underlying == nullptr || data == nullptr || !underlying->IsInteger()) {
        return std::nullopt;
    }
    return static_cast<int>(underlying->GetSignedIntPropertyValue(data));
}

/** @brief 向已预检的枚举属性写入底层整数。 */
[[nodiscard]] auto write_enum(FEnumProperty* property, void* container, const int value) -> bool {
    auto* const underlying = property == nullptr ? nullptr : property->GetUnderlyingProperty();
    auto* const data =
        property == nullptr ? nullptr : property->ContainerPtrToValuePtr<void>(container);
    if (underlying == nullptr || data == nullptr || !underlying->IsInteger()) {
        return false;
    }
    underlying->SetIntPropertyValue(data, static_cast<int64_t>(value));
    return true;
}

/** @brief 读取已预检的 `uint8` 属性。 */
[[nodiscard]] auto read_byte(FByteProperty* property, const void* saveParam) -> int {
    return property->GetPropertyValueInContainer(saveParam);
}

/** @brief 把已裁剪的值写入预检通过的 `uint8` 属性。 */
auto write_byte(FByteProperty* property, void* saveParam, const int value) -> void {
    property->SetPropertyValueInContainer(saveParam, static_cast<std::uint8_t>(value));
}

struct RuntimeLimits {
    int condensationMaxStars{};
    int workSuitabilityMaxRank{};
};

/** @brief 从当前世界的 PalGameSetting 读取版本相关上限，不把数值硬编码进 Mod。 */
[[nodiscard]] auto runtime_limits(UObject* worldContext) -> std::optional<RuntimeLimits> {
    auto* const utility = pal_game::find_pal_utility();
    auto* const function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalUtility:GetGameSetting"));
    if (utility == nullptr || function == nullptr) {
        return std::nullopt;
    }
    auto* const context = CastField<FObjectPropertyBase>(
        function->FindProperty(FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const result = CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!pal_game::is_valid(worldContext) || !pal_game::has_exact_parameter_count(function, 2) ||
        !pal_game::is_input_parameter(context) || !pal_game::is_return_parameter(result)) {
        return std::nullopt;
    }
    FunctionParams params{function};
    context->SetObjectPropertyValue(context->ContainerPtrToValuePtr<void>(params.data()),
                                    worldContext);
    utility->ProcessEvent(function, params.data());
    auto* const setting =
        result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(params.data()));
    if (!pal_game::is_valid(setting)) {
        return std::nullopt;
    }
    auto* const characterMaxRank =
        CastField<FIntProperty>(setting->GetPropertyByNameInChain(STR("CharacterMaxRank")));
    auto* const workSuitabilityMaxRank =
        CastField<FIntProperty>(setting->GetPropertyByNameInChain(STR("WorkSuitabilityMaxRank")));
    if (characterMaxRank == nullptr || workSuitabilityMaxRank == nullptr) {
        return std::nullopt;
    }
    const int internalMaxRank = characterMaxRank->GetPropertyValueInContainer(setting);
    const int workMaxRank = workSuitabilityMaxRank->GetPropertyValueInContainer(setting);
    if (internalMaxRank < 1 || internalMaxRank > 100 || workMaxRank < 1 || workMaxRank > 100) {
        return std::nullopt;
    }
    return RuntimeLimits{
        .condensationMaxStars = internalMaxRank - 1,
        .workSuitabilityMaxRank = workMaxRank,
    };
}

/** @brief 读取持久化的个体工作适应性加成数组，并拒绝重复/越界条目。 */
[[nodiscard]] auto read_work_suitability_bonuses(UStruct* saveStruct, const void* saveParam,
                                                 const int maxRank)
    -> std::optional<WorkSuitabilityRanks> {
    auto* const arrayProperty = saveStruct == nullptr
                                    ? nullptr
                                    : CastField<FArrayProperty>(saveStruct->FindProperty(
                                          FName(STR("GotWorkSuitabilityAddRankList"), FNAME_Find)));
    auto* const itemProperty =
        arrayProperty == nullptr ? nullptr : CastField<FStructProperty>(arrayProperty->GetInner());
    auto* const itemStruct = itemProperty == nullptr ? nullptr : itemProperty->GetStruct().Get();
    auto* const suitability = find_enum(itemStruct, STR("WorkSuitability"));
    auto* const rank =
        itemStruct == nullptr
            ? nullptr
            : CastField<FIntProperty>(itemStruct->FindProperty(FName(STR("Rank"), FNAME_Find)));
    if (arrayProperty == nullptr || itemProperty == nullptr || suitability == nullptr ||
        rank == nullptr) {
        return std::nullopt;
    }

    WorkSuitabilityRanks values{};
    std::array<bool, kWorkSuitabilityCount> visited{};
    FScriptArrayHelper_InContainer entries(arrayProperty, saveParam);
    const int32 entryCount = entries.Num();
    if (entryCount < 0 || entryCount > kMaxWorkSuitabilityEntries) {
        return std::nullopt;
    }
    for (int32 index{}; index < entryCount; ++index) {
        void* const item = entries.GetRawPtr(index);
        const auto rawSuitability = read_enum(suitability, item);
        if (!rawSuitability.has_value() || *rawSuitability < 1 ||
            *rawSuitability > static_cast<int>(kWorkSuitabilityCount)) {
            return std::nullopt;
        }
        const auto valueIndex = static_cast<std::size_t>(*rawSuitability - 1);
        const int value = rank->GetPropertyValueInContainer(item);
        if (visited[valueIndex] || value < 0 || value > maxRank) {
            return std::nullopt;
        }
        visited[valueIndex] = true;
        values[valueIndex] = value;
    }
    return values;
}

/**
 * @brief 调用原生工作适应性 getter。
 * @details Palworld 1.0.2 中该 getter 的返回值**已包含** `GotWorkSuitabilityAddRankList` 的
 *          永久附加值与浓缩（character）加成；物种固有+浓缩 = 返回值 − 永久附加，只读。
 *          永久附加（bonus）才是玩家可编辑的部分，调整后 getter 会等量反映。
 *          优先尝试 Handle（`GetOuter()`），因为浓缩升星后 Handle 上的缓存比 Parameter 更及时刷新；
 *          Handle 不可用或没有该函数时透明回退到 Parameter。
 */
[[nodiscard]] auto work_suitability_base(UObject* pal, const WorkSuitability suitability)
    -> std::optional<int> {
    auto* const handle = pal != nullptr ? pal->GetOuterPrivate() : nullptr;
    const bool handleValid = handle != nullptr && pal_game::is_valid(handle);

    // Try Handle first
    UObject* callTarget = pal;
    UFunction* function = nullptr;
    if (handleValid) {
        function = handle->GetFunctionByNameInChain(STR("GetWorkSuitabilityRankWithCharacterRank"));
        if (function != nullptr) {
            callTarget = handle;
        }
    }
    // Fall back to Parameter
    if (function == nullptr) {
        function =
            pal != nullptr
                ? pal->GetFunctionByNameInChain(STR("GetWorkSuitabilityRankWithCharacterRank"))
                : nullptr;
        callTarget = pal;
    }

    auto* const input = function == nullptr ? nullptr : find_enum(function, STR("WorkSuitability"));
    auto* const output = function == nullptr ? nullptr
                                             : CastField<FIntProperty>(function->FindProperty(
                                                   FName(STR("ReturnValue"), FNAME_Find)));
    if (!pal_game::has_exact_parameter_count(function, 2) || !pal_game::is_input_parameter(input) ||
        !pal_game::is_return_parameter(output)) {
        return std::nullopt;
    }
    FunctionParams params{function};
    if (!write_enum(input, params.data(), static_cast<int>(suitability))) {
        return std::nullopt;
    }
    callTarget->ProcessEvent(function, params.data());
    const int value = output->GetPropertyValueInContainer(params.data());
    return value >= 0 && value <= 100 ? std::optional<int>{value} : std::nullopt;
}

/**
 * @brief 直接覆写 `GotWorkSuitabilityAddRankList` 中已有条目的 Rank 值。
 * @details 不调整数组大小——只修改已存在条目的值，不增删元素。需要新增条目的适应性
 *          仍由 `SetWorkSuitabilityAddRank` 以正增量完成。
 * @return 反射字段全部可用且写入成功时返回 `true`；结构不可用时返回 `false`。
 */
[[nodiscard]] auto write_work_suitability_bonuses_in_place(UStruct* saveStruct, void* saveParam,
                                                           const WorkSuitabilityRanks& bonuses,
                                                           const int maxRank) -> bool {
    auto* const arrayProperty = saveStruct == nullptr
                                    ? nullptr
                                    : CastField<FArrayProperty>(saveStruct->FindProperty(
                                          FName(STR("GotWorkSuitabilityAddRankList"), FNAME_Find)));
    auto* const itemProperty =
        arrayProperty == nullptr ? nullptr : CastField<FStructProperty>(arrayProperty->GetInner());
    auto* const itemStruct = itemProperty == nullptr ? nullptr : itemProperty->GetStruct().Get();
    auto* const suitability = find_enum(itemStruct, STR("WorkSuitability"));
    auto* const rank =
        itemStruct == nullptr
            ? nullptr
            : CastField<FIntProperty>(itemStruct->FindProperty(FName(STR("Rank"), FNAME_Find)));
    if (arrayProperty == nullptr || itemProperty == nullptr || suitability == nullptr ||
        rank == nullptr) {
        return false;
    }

    FScriptArrayHelper_InContainer entries(arrayProperty, saveParam);
    const int32 entryCount = entries.Num();
    if (entryCount < 0 || entryCount > kMaxWorkSuitabilityEntries) {
        return false;
    }

    // Build a map: suitability enum value → current array index
    int32 foundIndices[kMaxWorkSuitabilityEntries]{};
    for (int32 i{}; i < kMaxWorkSuitabilityEntries; ++i) {
        foundIndices[i] = -1;
    }
    for (int32 i{}; i < entryCount; ++i) {
        void* const item = entries.GetRawPtr(i);
        const auto rawSuitability = read_enum(suitability, item);
        if (!rawSuitability.has_value() || *rawSuitability < 1 ||
            *rawSuitability > kMaxWorkSuitabilityEntries) {
            return false;
        }
        foundIndices[*rawSuitability - 1] = i;
    }

    // Update existing entries in place
    for (std::size_t suitabilityIndex{}; suitabilityIndex < bonuses.size(); ++suitabilityIndex) {
        const int32 arrayIndex = foundIndices[suitabilityIndex];
        if (arrayIndex < 0) {
            continue;  // entry doesn't exist — handled by SetWorkSuitabilityAddRank
        }
        const int clamped = clamp_work_suitability_bonus(bonuses[suitabilityIndex], maxRank);
        void* const item = entries.GetRawPtr(arrayIndex);
        rank->SetPropertyValueInContainer(item, static_cast<int32_t>(clamped));
    }
    return true;
}

/** @brief 已完整预检的原生工作适应性永久加成增量接口；仅用于新增条目（正增量）。 */
class WorkSuitabilitySetter final {
public:
    [[nodiscard]] static auto prepare(UObject* pal) -> std::optional<WorkSuitabilitySetter> {
        auto* const function =
            pal == nullptr ? nullptr
                           : pal->GetFunctionByNameInChain(STR("SetWorkSuitabilityAddRank"));
        auto* const suitability =
            function == nullptr ? nullptr : find_enum(function, STR("WorkSuitability"));
        auto* const rank = function == nullptr ? nullptr
                                               : CastField<FIntProperty>(function->FindProperty(
                                                     FName(STR("addRank"), FNAME_Find)));
        if (!pal_game::has_exact_parameter_count(function, 2) ||
            !pal_game::is_input_parameter(suitability) || !pal_game::is_input_parameter(rank) ||
            function->GetReturnProperty() != nullptr) {
            return std::nullopt;
        }
        return WorkSuitabilitySetter{function, suitability, rank};
    }

    [[nodiscard]] auto add(UObject* pal, const WorkSuitability suitability, const int delta) const
        -> bool {
        if (!pal_game::is_valid(pal)) {
            return false;
        }
        FunctionParams params{function_};
        if (!write_enum(suitability_, params.data(), static_cast<int>(suitability))) {
            return false;
        }
        rank_->SetPropertyValueInContainer(params.data(), static_cast<int32_t>(delta));
        pal->ProcessEvent(function_, params.data());
        return true;
    }

private:
    WorkSuitabilitySetter(UFunction* function, FEnumProperty* suitability, FIntProperty* rank)
        : function_{function}, suitability_{suitability}, rank_{rank} {}

    UFunction* function_{};
    FEnumProperty* suitability_{};
    FIntProperty* rank_{};
};

/** @brief 通知游戏刷新由 SaveParameter 派生的缓存、委托和组件。 */
[[nodiscard]] auto invoke_save_parameter_rep(UObject* pal, UFunction* function) -> bool {
    if (!pal_game::is_valid(pal) || !pal_game::has_exact_parameter_count(function, 0) ||
        function->GetReturnProperty() != nullptr) {
        return false;
    }
    pal->ProcessEvent(function, nullptr);
    return true;
}

/** @brief 取得 `PalDatabaseCharacterParameter` 单例；不可用时返回 `nullptr`。 */
[[nodiscard]] auto database() -> UObject* {
    return UObjectGlobals::FindFirstOf(STR("PalDatabaseCharacterParameter"));
}

/**
 * @brief 调用 `UpdateApplyDatabaseToIndividualParameter` 刷新数据库派生属性。
 * @details 浓缩升星后 SaveParameter.Rank 已更新，但工作适应性等由数据库 Rank 推导的字段
 *          可能仍缓存旧值。此函数触发数据库重新评估当前 Rank 对应的全部派生数据。
 */
auto apply_database_to_parameter(UObject* pal) -> bool {
    auto* const db = database();
    auto* const function =
        pal_game::is_valid(db)
            ? db->GetFunctionByNameInChain(STR("UpdateApplyDatabaseToIndividualParameter"))
            : nullptr;
    auto* const input = function == nullptr ? nullptr
                                            : CastField<FObjectPropertyBase>(function->FindProperty(
                                                  FName(STR("IndividualParameter"), FNAME_Find)));
    if (!pal_game::is_valid(pal) || !pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_input_parameter(input) || function->GetReturnProperty() != nullptr) {
        return false;
    }
    FunctionParams params{function};
    input->SetObjectPropertyValue(input->ContainerPtrToValuePtr<void>(params.data()), pal);
    db->ProcessEvent(function, params.data());
    return true;
}

/**
 * @brief 查询某亲密度 rank 所需的累计点数阈值。
 * @details 使用动态参数缓冲区匹配 `GetFriendshipRequiredPointByRank(int32, int32&) -> bool`
 *          的 UFunction 布局，避免手工对齐；签名不匹配或数据库查询失败时返回空，调用方
 *          必须保持零写入。
 */
[[nodiscard]] auto friendship_required_point(const int rank) -> std::optional<int> {
    auto* const db = database();
    auto* const function =
        db == nullptr ? nullptr
                      : db->GetFunctionByNameInChain(STR("GetFriendshipRequiredPointByRank"));
    auto* const rankProp = function == nullptr ? nullptr
                                               : CastField<FIntProperty>(function->FindProperty(
                                                     FName(STR("FriendshipRank"), FNAME_Find)));
    auto* const outProp = function == nullptr ? nullptr
                                              : CastField<FIntProperty>(function->FindProperty(
                                                    FName(STR("OutRequiredPoint"), FNAME_Find)));
    auto* const resultProp =
        function == nullptr ? nullptr : CastField<FBoolProperty>(function->GetReturnProperty());
    // bool 返回 + 1 入参 + 1 出参 = 3 个 CPF_Parm；本项目计数含返回值（与 GetSkillData 一致）。
    if (!pal_game::has_exact_parameter_count(function, 3) ||
        !pal_game::is_input_parameter(rankProp) || !pal_game::is_output_parameter(outProp) ||
        resultProp == nullptr) {
        return std::nullopt;
    }
    FunctionParams params{function};
    rankProp->SetPropertyValueInContainer(params.data(), static_cast<int32_t>(rank));
    db->ProcessEvent(function, params.data());
    if (!resultProp->GetPropertyValueInContainer(params.data())) {
        return std::nullopt;
    }
    return outProp->GetPropertyValueInContainer(params.data());
}
}  // namespace

auto PalStatGateway::is_valid(const PalStatTarget target) const -> bool {
    return to_pal(target) != nullptr;
}

auto PalStatGateway::read_stats(const PalStatTarget target) -> PalStatSnapshot {
    PalStatSnapshot snapshot;
    auto* const pal = to_pal(target);
    if (pal == nullptr) {
        return snapshot;
    }
    const auto level = pal_game::invoke<int>(pal, STR("GetLevel"));
    const auto friendshipRank = pal_game::invoke<int>(pal, STR("GetFriendshipRank"));
    const auto friendshipPoint = pal_game::invoke<int>(pal, STR("GetFriendshipPoint"));
    const auto limits = runtime_limits(pal);
    if (!level.has_value() || !friendshipRank.has_value() || !friendshipPoint.has_value() ||
        !limits.has_value()) {
        return snapshot;
    }

    FStructProperty* saveProperty = nullptr;
    void* const saveParam = save_parameter_slot(pal, saveProperty);
    if (saveParam == nullptr) {
        return snapshot;
    }
    auto* const rowStruct = saveProperty->GetStruct().Get();
    auto* const talentHp = find_byte(rowStruct, STR("Talent_HP"));
    auto* const talentShot = find_byte(rowStruct, STR("Talent_Shot"));
    auto* const talentDefense = find_byte(rowStruct, STR("Talent_Defense"));
    auto* const soulHpRank = find_byte(rowStruct, STR("Rank_HP"));
    auto* const soulAttackRank = find_byte(rowStruct, STR("Rank_Attack"));
    auto* const soulDefenseRank = find_byte(rowStruct, STR("Rank_Defence"));
    auto* const soulWorkSpeedRank = find_byte(rowStruct, STR("Rank_CraftSpeed"));
    auto* const condensationRank = find_byte(rowStruct, STR("Rank"));
    auto* const rankUpExp = find_uint16(rowStruct, STR("RankUpExp"));
    auto* const gender = find_enum(rowStruct, STR("Gender"));
    if (talentHp == nullptr || talentShot == nullptr || talentDefense == nullptr ||
        soulHpRank == nullptr || soulAttackRank == nullptr || soulDefenseRank == nullptr ||
        soulWorkSpeedRank == nullptr || condensationRank == nullptr || rankUpExp == nullptr ||
        gender == nullptr) {
        return snapshot;
    }

    const int internalRank = read_byte(condensationRank, saveParam);
    const auto rawGender = read_enum(gender, saveParam);
    const auto workBonuses =
        read_work_suitability_bonuses(rowStruct, saveParam, limits->workSuitabilityMaxRank);
    if (internalRank < 1 || internalRank > limits->condensationMaxStars + 1 ||
        !rawGender.has_value() || *rawGender < static_cast<int>(PalGender::none) ||
        *rawGender > static_cast<int>(PalGender::female) || !workBonuses.has_value()) {
        return snapshot;
    }

    // Try to read CharacterID for potential database-driven work suitability lookup.
    auto* const characterIdProp =
        CastField<FNameProperty>(rowStruct->FindProperty(FName(STR("CharacterID"), FNAME_Find)));
    const bool hasCharId = characterIdProp != nullptr;
    const FName characterId =
        hasCharId ? characterIdProp->GetPropertyValueInContainer(saveParam) : FName{};

    WorkSuitabilityRanks workBases{};
    WorkSuitabilityRanks workTotals{};
    for (std::size_t index{}; index < workBases.size(); ++index) {
        const auto suitability = static_cast<WorkSuitability>(index + 1);
        // work_suitability_base calls GetWorkSuitabilityRankWithCharacterRank, which
        // includes the condensation (character) rank bonus that GetWorkSuitabilityRank omits.
        const auto paramBase = work_suitability_base(pal, suitability);
        if (!paramBase.has_value()) {
            return snapshot;
        }
        workBases[index] = *paramBase;
        workTotals[index] = work_suitability_total_rank(workBases[index], (*workBonuses)[index],
                                                        limits->workSuitabilityMaxRank);
    }

    // Diagnostic: log CharacterID being read to help troubleshoot database lookup.
    if (hasCharId) {
        Output::send<LogLevel::Verbose>(STR("PalworldEditor: CharacterID={} rank={}\n"),
                                        characterId.ToString().c_str(), internalRank);
    }

    // Diagnostic: log raw GetWorkSuitabilityRank values alongside the internal Rank.
    Output::send<LogLevel::Verbose>(
        STR("PalworldEditor: read_stats(rank={} maxWS={}) "
            "base_ws=[{},{},{},{},{},{},{},{},{},{},{},{},{}] "
            "bonus=[{},{},{},{},{},{},{},{},{},{},{},{},{}]\n"),
        internalRank, limits->workSuitabilityMaxRank, workBases[0], workBases[1], workBases[2],
        workBases[3], workBases[4], workBases[5], workBases[6], workBases[7], workBases[8],
        workBases[9], workBases[10], workBases[11], workBases[12], (*workBonuses)[0],
        (*workBonuses)[1], (*workBonuses)[2], (*workBonuses)[3], (*workBonuses)[4],
        (*workBonuses)[5], (*workBonuses)[6], (*workBonuses)[7], (*workBonuses)[8],
        (*workBonuses)[9], (*workBonuses)[10], (*workBonuses)[11], (*workBonuses)[12]);

    snapshot.level = *level;
    snapshot.friendshipRank = *friendshipRank;
    snapshot.friendshipPoint = *friendshipPoint;
    snapshot.talentHp = read_byte(talentHp, saveParam);
    snapshot.talentShot = read_byte(talentShot, saveParam);
    snapshot.talentDefense = read_byte(talentDefense, saveParam);
    snapshot.soulHpRank = read_byte(soulHpRank, saveParam);
    snapshot.soulAttackRank = read_byte(soulAttackRank, saveParam);
    snapshot.soulDefenseRank = read_byte(soulDefenseRank, saveParam);
    snapshot.soulWorkSpeedRank = read_byte(soulWorkSpeedRank, saveParam);
    snapshot.condensationStars =
        internal_rank_to_condensation_stars(internalRank, limits->condensationMaxStars);
    snapshot.condensationMaxStars = limits->condensationMaxStars;
    snapshot.partnerSkillLevel = internalRank;
    snapshot.rankUpExp = rankUpExp->GetPropertyValueInContainer(saveParam);
    snapshot.gender = static_cast<PalGender>(*rawGender);
    snapshot.workSuitabilityMaxRank = limits->workSuitabilityMaxRank;
    snapshot.workSuitabilityBaseRanks = workBases;
    snapshot.workSuitabilityBonusRanks = *workBonuses;
    snapshot.workSuitabilityTotalRanks = workTotals;
    snapshot.readable = true;
    return snapshot;
}

auto PalStatGateway::apply_stat_edit(const PalStatTarget target, const PalStatEditRequest& request)
    -> PalStatEditResult {
    PalStatEditResult result;
    auto* const pal = to_pal(target);
    if (pal == nullptr || !has_any_change(request.values)) {
        result.message = "属性修改已拒绝：目标无效或请求没有变化。";
        return result;
    }

    const auto before = read_stats(target);
    if (!before.readable) {
        result.status = PalStatEditStatus::preflightFailed;
        result.message = "属性修改未执行：无法完整读取修改前快照。";
        return result;
    }

    PalStatValues expectedValues = request.values;
    if (expectedValues.workSuitabilityBonusRanks.has_value()) {
        for (std::size_t index{}; index < expectedValues.workSuitabilityBonusRanks->size();
             ++index) {
            const int maxBonus = max_editable_work_suitability_bonus(
                before.workSuitabilityBaseRanks[index], before.workSuitabilityBonusRanks[index],
                before.workSuitabilityMaxRank);
            (*expectedValues.workSuitabilityBonusRanks)[index] =
                std::clamp((*expectedValues.workSuitabilityBonusRanks)[index], 0, maxBonus);
        }
    }

    FStructProperty* saveProperty = nullptr;
    void* const saveParam = save_parameter_slot(pal, saveProperty);
    if (saveParam == nullptr) {
        result.status = PalStatEditStatus::preflightFailed;
        result.snapshot = before;
        result.message = "属性修改未执行：SaveParameter 结构不可用。";
        return result;
    }
    auto* const rowStruct = saveProperty->GetStruct().Get();

    auto* const level =
        request.values.level.has_value() ? find_byte(rowStruct, STR("Level")) : nullptr;
    auto* const talentHp =
        request.values.talentHp.has_value() ? find_byte(rowStruct, STR("Talent_HP")) : nullptr;
    auto* const talentShot =
        request.values.talentShot.has_value() ? find_byte(rowStruct, STR("Talent_Shot")) : nullptr;
    auto* const talentDefense = request.values.talentDefense.has_value()
                                    ? find_byte(rowStruct, STR("Talent_Defense"))
                                    : nullptr;
    auto* const soulHpRank =
        request.values.soulHpRank.has_value() ? find_byte(rowStruct, STR("Rank_HP")) : nullptr;
    auto* const soulAttackRank = request.values.soulAttackRank.has_value()
                                     ? find_byte(rowStruct, STR("Rank_Attack"))
                                     : nullptr;
    auto* const soulDefenseRank = request.values.soulDefenseRank.has_value()
                                      ? find_byte(rowStruct, STR("Rank_Defence"))
                                      : nullptr;
    auto* const soulWorkSpeedRank = request.values.soulWorkSpeedRank.has_value()
                                        ? find_byte(rowStruct, STR("Rank_CraftSpeed"))
                                        : nullptr;
    auto* const condensationRank =
        request.values.condensationStars.has_value() ? find_byte(rowStruct, STR("Rank")) : nullptr;
    auto* const rankUpExp = request.values.condensationStars.has_value()
                                ? find_uint16(rowStruct, STR("RankUpExp"))
                                : nullptr;
    auto* const gender =
        request.values.gender.has_value() ? find_enum(rowStruct, STR("Gender")) : nullptr;
    auto* const friendshipPoint = request.values.friendshipRank.has_value()
                                      ? CastField<FIntProperty>(rowStruct->FindProperty(
                                            FName(STR("FriendshipPoint"), FNAME_Find)))
                                      : nullptr;
    const auto requiredFriendshipPoint =
        request.values.friendshipRank.has_value()
            ? friendship_required_point(clamp_friendship_rank(*request.values.friendshipRank))
            : std::optional<int>{};
    const bool hasWorkChange = request.values.workSuitabilityBonusRanks.has_value();
    const bool needsSaveParameterRefresh = has_core_stat_change(request.values) || hasWorkChange;
    auto* const onRepSaveParameter = needsSaveParameterRefresh
                                         ? pal->GetFunctionByNameInChain(STR("OnRep_SaveParameter"))
                                         : nullptr;
    // Also try the Handle — its OnRep may refresh work-suitability caches that the Parameter's
    // implementation leaves stale.
    auto* const handle = pal->GetOuterPrivate();
    auto* const onRepSaveParameterHandle =
        needsSaveParameterRefresh && handle != nullptr && pal_game::is_valid(handle)
            ? handle->GetFunctionByNameInChain(STR("OnRep_SaveParameter"))
            : nullptr;
    // WorkSuitabilitySetter is only used to ADD new entries (positive delta);
    // decreases and in-place modifications are handled by write_work_suitability_bonuses_in_place.
    const auto workSetter = hasWorkChange ? WorkSuitabilitySetter::prepare(pal)
                                          : std::optional<WorkSuitabilitySetter>{};

    const bool preflightFailed =
        (request.values.level.has_value() && level == nullptr) ||
        (request.values.talentHp.has_value() && talentHp == nullptr) ||
        (request.values.talentShot.has_value() && talentShot == nullptr) ||
        (request.values.talentDefense.has_value() && talentDefense == nullptr) ||
        (request.values.soulHpRank.has_value() && soulHpRank == nullptr) ||
        (request.values.soulAttackRank.has_value() && soulAttackRank == nullptr) ||
        (request.values.soulDefenseRank.has_value() && soulDefenseRank == nullptr) ||
        (request.values.soulWorkSpeedRank.has_value() && soulWorkSpeedRank == nullptr) ||
        (needsSaveParameterRefresh &&
         (onRepSaveParameter == nullptr || onRepSaveParameter->GetParmsSize() != 0)) ||
        (request.values.condensationStars.has_value() &&
         (condensationRank == nullptr || rankUpExp == nullptr)) ||
        (request.values.gender.has_value() &&
         (gender == nullptr || !is_editable_gender(*request.values.gender))) ||
        (hasWorkChange && (rowStruct->FindProperty(FName(STR("GotWorkSuitabilityAddRankList"),
                                                         FNAME_Find)) == nullptr ||
                           !workSetter.has_value())) ||
        (request.values.friendshipRank.has_value() &&
         (friendshipPoint == nullptr || !requiredFriendshipPoint.has_value()));
    if (preflightFailed) {
        result.status = PalStatEditStatus::preflightFailed;
        result.snapshot = before;
        result.message = "属性修改未执行：当前游戏版本的字段、原生接口或请求值不可安全使用。";
        return result;
    }

    if (level != nullptr) {
        write_byte(level, saveParam, clamp_level(*request.values.level));
    }
    if (talentHp != nullptr) {
        write_byte(talentHp, saveParam, clamp_talent(*request.values.talentHp));
    }
    if (talentShot != nullptr) {
        write_byte(talentShot, saveParam, clamp_talent(*request.values.talentShot));
    }
    if (talentDefense != nullptr) {
        write_byte(talentDefense, saveParam, clamp_talent(*request.values.talentDefense));
    }
    if (soulHpRank != nullptr) {
        write_byte(soulHpRank, saveParam, clamp_soul_rank(*request.values.soulHpRank));
    }
    if (soulAttackRank != nullptr) {
        write_byte(soulAttackRank, saveParam, clamp_soul_rank(*request.values.soulAttackRank));
    }
    if (soulDefenseRank != nullptr) {
        write_byte(soulDefenseRank, saveParam, clamp_soul_rank(*request.values.soulDefenseRank));
    }
    if (soulWorkSpeedRank != nullptr) {
        write_byte(soulWorkSpeedRank, saveParam,
                   clamp_soul_rank(*request.values.soulWorkSpeedRank));
    }
    bool writesCompleted = true;
    if (condensationRank != nullptr) {
        write_byte(condensationRank, saveParam,
                   condensation_stars_to_internal_rank(*request.values.condensationStars,
                                                       before.condensationMaxStars));
        rankUpExp->SetPropertyValueInContainer(saveParam, static_cast<std::uint16_t>(0));
        // Refresh database-derived properties (work suitability etc.) at the new Rank.
        // Must happen before OnRep_SaveParameter so the notify finds consistent state.
        writesCompleted = apply_database_to_parameter(pal) && writesCompleted;
    }
    if (gender != nullptr) {
        writesCompleted = write_enum(gender, saveParam, static_cast<int>(*request.values.gender));
    }
    if (hasWorkChange) {
        // Phase 1: in-place modification for EXISTING entries（增大、减小或清零）
        writesCompleted = write_work_suitability_bonuses_in_place(
                              rowStruct, saveParam, *expectedValues.workSuitabilityBonusRanks,
                              before.workSuitabilityMaxRank) &&
                          writesCompleted;
        // Phase 2: SetWorkSuitabilityAddRank ONLY for NEW entries（before 为 0 且期望 > 0）。
        // Phase 1 already wrote the correct value for existing entries; double-applying
        // a delta on top would corrupt the value.
        if (writesCompleted) {
            for (std::size_t index{}; index < expectedValues.workSuitabilityBonusRanks->size();
                 ++index) {
                const bool isNewEntry = before.workSuitabilityBonusRanks[index] == 0;
                const int expected = (*expectedValues.workSuitabilityBonusRanks)[index];
                if (!isNewEntry || expected <= 0) {
                    continue;
                }
                writesCompleted =
                    workSetter->add(pal, static_cast<WorkSuitability>(index + 1), expected);
                if (!writesCompleted) {
                    break;
                }
            }
        }
    }
    if (friendshipPoint != nullptr) {
        friendshipPoint->SetPropertyValueInContainer(
            saveParam, static_cast<int32_t>(*requiredFriendshipPoint));
    }
    if (needsSaveParameterRefresh) {
        writesCompleted = invoke_save_parameter_rep(pal, onRepSaveParameter) && writesCompleted;
        if (onRepSaveParameterHandle != nullptr) {
            writesCompleted =
                invoke_save_parameter_rep(handle, onRepSaveParameterHandle) && writesCompleted;
        }
    }

    result.snapshot = read_stats(target);
    const bool rankProgressNormalized =
        !request.values.condensationStars.has_value() || result.snapshot.rankUpExp == 0;
    if (writesCompleted && rankProgressNormalized &&
        verify_stat_edit(expectedValues, result.snapshot)) {
        result.status = PalStatEditStatus::succeeded;
        result.message = "属性修改成功，并已通过游戏数据重读验证。";
        return result;
    }

    if (level != nullptr) {
        write_byte(level, saveParam, before.level);
    }
    if (talentHp != nullptr) {
        write_byte(talentHp, saveParam, before.talentHp);
    }
    if (talentShot != nullptr) {
        write_byte(talentShot, saveParam, before.talentShot);
    }
    if (talentDefense != nullptr) {
        write_byte(talentDefense, saveParam, before.talentDefense);
    }
    if (soulHpRank != nullptr) {
        write_byte(soulHpRank, saveParam, before.soulHpRank);
    }
    if (soulAttackRank != nullptr) {
        write_byte(soulAttackRank, saveParam, before.soulAttackRank);
    }
    if (soulDefenseRank != nullptr) {
        write_byte(soulDefenseRank, saveParam, before.soulDefenseRank);
    }
    if (soulWorkSpeedRank != nullptr) {
        write_byte(soulWorkSpeedRank, saveParam, before.soulWorkSpeedRank);
    }
    if (condensationRank != nullptr) {
        write_byte(condensationRank, saveParam,
                   condensation_stars_to_internal_rank(before.condensationStars,
                                                       before.condensationMaxStars));
        rankUpExp->SetPropertyValueInContainer(saveParam,
                                               static_cast<std::uint16_t>(before.rankUpExp));
        static_cast<void>(apply_database_to_parameter(pal));
    }
    bool rollbackOperationsSucceeded = true;
    if (gender != nullptr) {
        rollbackOperationsSucceeded =
            write_enum(gender, saveParam, static_cast<int>(before.gender));
    }
    if (hasWorkChange) {
        // In-place restore of pre-edit values.  All entries that the edit touched
        // (including newly-created ones and those set to 0) exist in the array at
        // this point — the in-place write handles every case without needing a
        // secondary SetWorkSuitabilityAddRank pass.
        rollbackOperationsSucceeded = write_work_suitability_bonuses_in_place(
                                          rowStruct, saveParam, before.workSuitabilityBonusRanks,
                                          before.workSuitabilityMaxRank) &&
                                      rollbackOperationsSucceeded;
    }
    if (friendshipPoint != nullptr) {
        friendshipPoint->SetPropertyValueInContainer(saveParam,
                                                     static_cast<int32_t>(before.friendshipPoint));
    }
    if (needsSaveParameterRefresh) {
        rollbackOperationsSucceeded =
            invoke_save_parameter_rep(pal, onRepSaveParameter) && rollbackOperationsSucceeded;
        if (onRepSaveParameterHandle != nullptr) {
            rollbackOperationsSucceeded =
                invoke_save_parameter_rep(handle, onRepSaveParameterHandle) &&
                rollbackOperationsSucceeded;
        }
    }

    result.snapshot = read_stats(target);
    PalStatValues rollbackExpected;
    if (request.values.level.has_value()) {
        rollbackExpected.level = before.level;
    }
    if (request.values.talentHp.has_value()) {
        rollbackExpected.talentHp = before.talentHp;
    }
    if (request.values.talentShot.has_value()) {
        rollbackExpected.talentShot = before.talentShot;
    }
    if (request.values.talentDefense.has_value()) {
        rollbackExpected.talentDefense = before.talentDefense;
    }
    if (request.values.soulHpRank.has_value()) {
        rollbackExpected.soulHpRank = before.soulHpRank;
    }
    if (request.values.soulAttackRank.has_value()) {
        rollbackExpected.soulAttackRank = before.soulAttackRank;
    }
    if (request.values.soulDefenseRank.has_value()) {
        rollbackExpected.soulDefenseRank = before.soulDefenseRank;
    }
    if (request.values.soulWorkSpeedRank.has_value()) {
        rollbackExpected.soulWorkSpeedRank = before.soulWorkSpeedRank;
    }
    if (request.values.condensationStars.has_value()) {
        rollbackExpected.condensationStars = before.condensationStars;
    }
    if (request.values.gender.has_value()) {
        rollbackExpected.gender = before.gender;
    }
    if (request.values.workSuitabilityBonusRanks.has_value()) {
        rollbackExpected.workSuitabilityBonusRanks = before.workSuitabilityBonusRanks;
    }
    if (request.values.friendshipRank.has_value()) {
        rollbackExpected.friendshipRank = before.friendshipRank;
    }
    const bool rankProgressRestored = !request.values.condensationStars.has_value() ||
                                      result.snapshot.rankUpExp == before.rankUpExp;
    if (rollbackOperationsSucceeded && rankProgressRestored &&
        verify_stat_edit(rollbackExpected, result.snapshot)) {
        result.status = PalStatEditStatus::verificationFailed;
        result.message = "属性写入后验证失败，已恢复修改前数值。";
    } else {
        result.status = PalStatEditStatus::rollbackFailed;
        result.message = "属性写入和恢复验证均失败；请立即退出当前世界并检查存档。";
    }
    return result;
}
}  // namespace pal_stats
