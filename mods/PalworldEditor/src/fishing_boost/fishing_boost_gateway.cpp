/**
 * @file fishing_boost_gateway.cpp
 * @brief 实现 UPalFishingSystem.CatchBattleParameter 的读写与可逆恢复。
 */
#include <vector>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/game_reflection.hpp>
#include <common/text_encoding.hpp>
#include <fishing_boost/fishing_boost_gateway.hpp>

namespace fishing_boost {
using namespace RC;
using namespace RC::Unreal;

namespace {

/**
 * @brief 解析属于当前世界的 UPalFishingSystem 实例。
 * @details 有世界锚时只接受唯一的同世界实例；无锚时只接受唯一有效实例。候选
 *          缺失或存在歧义时 fail-closed，InternalIndex 不能代替世界身份。FindAllOf
 *          保留 UE4SS 的父类链匹配并排除 CDO/archetype；Palworld 通过
 *          FishingSystemClass 配置 Blueprint 派生实现，不能用精确类名过滤。
 *          只在触发沿/有界重试调用。
 */
[[nodiscard]] auto resolve_system(UObject* worldContext) -> UObject* {
    auto* const expectedWorld =
        pal_game::is_valid(worldContext) ? worldContext->GetWorld() : nullptr;
    UObject* matched{};
    std::optional<SystemCandidateRank> selectedRank;
    std::size_t candidateCount{};
    std::size_t matchingExpectedWorldCount{};
    std::vector<UObject*> candidates;
    UObjectGlobals::FindAllOf(STR("PalFishingSystem"), candidates);
    for (auto* const obj : candidates) {
        if (!pal_game::is_valid(obj)) {
            continue;
        }
        const auto* const item = UObjectArray::IndexToObject(obj->GetInternalIndex());
        if (item == nullptr || item->IsPendingKill()) {
            continue;
        }
        auto* const candidateWorld = obj->GetWorld();
        const SystemCandidateRank rank{
            .matchesExpectedWorld = expectedWorld != nullptr && candidateWorld == expectedWorld,
            .internalIndex = obj->GetInternalIndex(),
        };
        ++candidateCount;
        if (rank.matchesExpectedWorld) {
            ++matchingExpectedWorldCount;
        }
        if (should_select_system_candidate(rank, selectedRank)) {
            selectedRank = rank;
            matched = obj;
        }
    }
    if (!is_system_candidate_selection_unambiguous(expectedWorld != nullptr, candidateCount,
                                                   matchingExpectedWorldCount)) {
        return nullptr;
    }
    return pal_game::is_valid(matched) ? matched : nullptr;
}

/**
 * @brief 找到 CatchBattleParameter 内的某个 float 字段。
 * @return 属性指针 + 容器指针（CatchBattleParameter 实例地址）。
 */
struct FieldAccess {
    FFloatProperty* property{};
    void* container{};
};

[[nodiscard]] auto find_field(UObject* system, const char* fieldName) -> FieldAccess {
    auto* const structProp =
        CastField<FStructProperty>(system->GetPropertyByNameInChain(STR("CatchBattleParameter")));
    // 外层结构必须精确匹配身份与大小（19×float；Dump/CXXHeaderDump/Pal.hpp 的
    // FPalFishingCatchBattleParameter）：同名异构的结构（PalSchema 替换等）不接受，
    // 布局漂移 fail-closed 而不是写入语义未知的字段。
    if (!pal_game::matches_struct_identity(structProp, STR("PalFishingCatchBattleParameter"),
                                           0x4C)) {
        return {};
    }
    auto* const structType = structProp->GetStruct().Get();
    const auto wideName = text_encoding::widen_ascii(fieldName);
    auto* const floatProp =
        CastField<FFloatProperty>(structType->FindProperty(FName(wideName.c_str(), FNAME_Find)));
    if (floatProp == nullptr) {
        return {};
    }
    return {.property = floatProp, .container = structProp->ContainerPtrToValuePtr<void>(system)};
}

}  // namespace

/** @brief 解析当前世界的锚（非 pending-kill 的 inventory）并派生本地玩家控制器。
 *  @details 裸 FindFirstOf(inventory) 在 LoadMap 的 GC 窗口期可能返回旧世界实例；
 *           GetLocalPalPlayerController 把传入对象原样作为 WorldContextObject（在
 *           给定世界上解析，不是全局找当前玩家），因此必须先过滤旧实例——旧世界
 *           对象被标记 RF_PendingKill 后即被排除；按内部注册序号显式选择最新候选，
 *           不依赖 ForEachUObject 的遍历方向。只在触发沿/有界重试调用。 */
[[nodiscard]] auto resolve_world_anchor() -> UObject* {
    UObject* candidate{};
    std::optional<SystemCandidateRank> selectedRank;
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
        auto* const cls = obj->GetClassPrivate();
        if (cls == nullptr || cls->GetName() != STR("PalPlayerInventoryData")) {
            return LoopAction::Continue;
        }
        const auto* const item = UObjectArray::IndexToObject(obj->GetInternalIndex());
        if (item == nullptr || item->IsPendingKill()) {
            return LoopAction::Continue;  // 旧世界待回收实例。
        }
        const SystemCandidateRank rank{.internalIndex = obj->GetInternalIndex()};
        if (should_select_system_candidate(rank, selectedRank)) {
            selectedRank = rank;
            candidate = obj;
        }
        return LoopAction::Continue;
    });
    return candidate == nullptr ? nullptr : pal_game::local_player_controller(candidate);
}

auto apply(Ledger& ledger, UObject* worldContext) -> GatewayStatus {
    auto* const system = resolve_system(worldContext);
    if (system == nullptr) {
        return GatewayStatus::targetUnavailable;
    }

    // 阶段一（预检）：解析并快照全部字段，任一缺失或类型漂移即整笔拒绝——结构
    // 不兼容的运行时不得被触碰（AGENTS.md：缺少读取能力时禁止开始写入）。
    std::array<FieldAccess, kFieldCount> accesses{};
    std::array<float, kFieldCount> originals{};
    for (std::size_t i{}; i < kFieldCount; ++i) {
        accesses[i] = find_field(system, kFieldCatalog[i].fieldName);
        if (accesses[i].property == nullptr) {
            ledger.disable_for_world();
            return GatewayStatus::preflightFailed;
        }
        originals[i] = accesses[i].property->GetPropertyValueInContainer(accesses[i].container);
    }

    // 阶段二（写入）：失败时尽力回滚并逐字段重读验证。全部回滚通过则无残留，
    // 仅停用域；任一回滚验证失败则把完整快照记入账本——恢复采用条件写回
    // （仅当前值仍等于本功能的覆盖值才恢复），全量记录不会误伤未污染字段。
    const auto abortWith = [&](const std::size_t writtenCount,
                               const GatewayStatus status) -> GatewayStatus {
        bool rollbackVerified = true;
        for (std::size_t j{}; j < writtenCount; ++j) {
            const auto rollback = find_field(system, kFieldCatalog[j].fieldName);
            if (rollback.property == nullptr) {
                rollbackVerified = false;
                continue;
            }
            rollback.property->SetPropertyValueInContainer(rollback.container, originals[j]);
            if (rollback.property->GetPropertyValueInContainer(rollback.container) !=
                originals[j]) {
                rollbackVerified = false;
            }
        }
        if (!rollbackVerified) {
            // 回滚无法验证：保留恢复责任（shutdown/LoadMap 的条件恢复会接手），
            // 同时安全停用域（AGENTS.md：回滚无法验证时保留恢复责任并停用）。
            ledger.record_originals(originals);
        }
        ledger.disable_for_world();
        // 未验证的回滚不得伪装成已验证结果：结果分类保持"已验证回滚"与
        // "回滚失败"可区分（AGENTS.md 事务结果分类契约）。
        return rollbackVerified ? status : GatewayStatus::rollbackFailed;
    };

    for (std::size_t i{}; i < kFieldCount; ++i) {
        accesses[i].property->SetPropertyValueInContainer(accesses[i].container,
                                                          kFieldCatalog[i].overrideValue);
        if (accesses[i].property->GetPropertyValueInContainer(accesses[i].container) !=
            kFieldCatalog[i].overrideValue) {
            return abortWith(i + 1, GatewayStatus::verificationFailed);
        }
    }
    ledger.record_originals(originals);
    return GatewayStatus::succeeded;
}

auto restore(Ledger& ledger, UObject* worldContext) -> GatewayStatus {
    const auto originals = ledger.originals();
    if (!originals.has_value()) {
        return GatewayStatus::succeeded;
    }
    // 锚无效 ≠ 目标不存在：无法解析（unknown）与确认不存在（confirmed absent）
    // 必须区分——锚处于 GC 边缘或尚未重建时，被覆盖的子系统可能完好存在，此刻
    // 清账即放弃真实存在的恢复责任。保留账本按瞬态重试，拿到确认才解除责任。
    auto* const system = resolve_system(worldContext);
    if (system == nullptr) {
        if (!pal_game::is_valid(worldContext) || worldContext->GetWorld() == nullptr) {
            return GatewayStatus::targetUnavailable;
        }
        // 锚有效而当前世界无匹配实例 = 确认不存在：覆盖值随旧世界对象销毁，
        // 新世界天然使用原生值，恢复责任可解除。
        ledger.clear_records();
        return GatewayStatus::succeeded;
    }

    // 阶段一（预检）：解析并读取全部未退役字段，任一缺失或类型漂移即整笔拒绝
    // （零写入）——逐字段交织会在后续字段漂移时留下"部分恢复"的中间态，违反
    // AGENTS.md"预检并读取原值→才开始写入"的契约。
    std::array<FieldAccess, kFieldCount> accesses{};
    std::array<float, kFieldCount> currents{};
    for (std::size_t i{}; i < kFieldCount; ++i) {
        if (ledger.is_field_retired(i)) {
            continue;  // 此前恢复时已确认责任消失的字段：重试永久跳过。
        }
        accesses[i] = find_field(system, kFieldCatalog[i].fieldName);
        if (accesses[i].property == nullptr) {
            ledger.disable_for_world();
            return GatewayStatus::preflightFailed;
        }
        currents[i] = accesses[i].property->GetPropertyValueInContainer(accesses[i].container);
    }

    // 阶段二（条件恢复）：仅当预检读到的当前值仍等于本功能写入的覆盖值才写回
    // 原值。被游戏或其他 mod 改过的字段（当前值 != 覆盖值）视为恢复责任已
    // 消失——立即退役，不得用陈旧快照覆盖更新后的值，也不得在后续重试中因
    // 值偶然回到覆盖值而重新捡起责任。恢复验证成功的字段同样立即退役。
    for (std::size_t i{}; i < kFieldCount; ++i) {
        if (ledger.is_field_retired(i) || accesses[i].property == nullptr) {
            continue;
        }
        if (currents[i] != kFieldCatalog[i].overrideValue) {
            ledger.retire_field(i);
            continue;
        }
        accesses[i].property->SetPropertyValueInContainer(accesses[i].container, (*originals)[i]);
        if (accesses[i].property->GetPropertyValueInContainer(accesses[i].container) !=
            (*originals)[i]) {
            ledger.disable_for_world();
            return GatewayStatus::rollbackFailed;
        }
        // 责任已履行：立即退役，后续字段失败保留账本时该字段不随快照滞留——
        // 否则重试窗口内值若被外部改回覆盖值，会被误判为残留而用陈旧快照覆盖。
        ledger.retire_field(i);
    }
    ledger.clear_records();
    return GatewayStatus::succeeded;
}

}  // namespace fishing_boost
