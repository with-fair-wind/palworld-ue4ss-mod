/**
 * @file fishing_boost_gateway.cpp
 * @brief 实现 UPalFishingSystem.CatchBattleParameter 的读写与可逆恢复。
 */
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
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
 * @details FindFirstOf 不绑定世界：LoadMap 的 GC 窗口期新旧实例并存时可能选中
 *          旧世界对象（改错实例却对新世界报告 active）。按 GetWorld() 与
 *          worldContext 比对过滤候选；无匹配返回空。只在触发沿/有界重试调用，
 *          遍历频率符合性能契约。
 */
[[nodiscard]] auto resolve_system(UObject* worldContext) -> UObject* {
    if (!pal_game::is_valid(worldContext)) {
        return nullptr;
    }
    auto* const expectedWorld = worldContext->GetWorld();
    if (expectedWorld == nullptr) {
        // 世界拆除/重建期间的 detached 锚：无世界可比对。若继续以 null 匹配，
        // 会命中同样无世界的类默认对象（CDO）——apply 会改写 CDO 并被新世界
        // 实例继承。按 targetUnavailable 走有界重试。
        return nullptr;
    }
    UObject* matched{};
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
        auto* const cls = obj->GetClassPrivate();
        if (cls == nullptr || cls->GetName() != STR("PalFishingSystem")) {
            return LoopAction::Continue;
        }
        if (obj->GetWorld() != expectedWorld) {
            // 旧世界待 GC 实例、其他世界实例，以及无世界的 CDO/模板对象
            // （其 GetWorld 为 null，expectedWorld 非空时恒不相等）。
            return LoopAction::Continue;
        }
        matched = obj;
        return LoopAction::Break;
    });
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
    if (!pal_game::is_valid(worldContext) || worldContext->GetWorld() == nullptr) {
        return GatewayStatus::targetUnavailable;
    }
    auto* const system = resolve_system(worldContext);
    if (system == nullptr) {
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
