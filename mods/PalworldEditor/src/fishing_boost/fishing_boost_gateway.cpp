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

/** @brief 解析 UPalFishingSystem 世界子系统。 */
[[nodiscard]] auto resolve_system() -> UObject* {
    auto* const system = UObjectGlobals::FindFirstOf(STR("PalFishingSystem"));
    return pal_game::is_valid(system) ? system : nullptr;
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
    if (structProp == nullptr || structProp->GetStruct().Get() == nullptr) {
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

auto apply(Ledger& ledger) -> GatewayStatus {
    auto* const system = resolve_system();
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
        return status;
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

auto restore(Ledger& ledger) -> GatewayStatus {
    const auto originals = ledger.originals();
    if (!originals.has_value()) {
        return GatewayStatus::succeeded;
    }
    auto* const system = resolve_system();
    if (system == nullptr) {
        // 对象已随世界销毁；新世界用原生值。
        ledger.clear_records();
        return GatewayStatus::succeeded;
    }

    for (std::size_t i{}; i < kFieldCount; ++i) {
        const auto access = find_field(system, kFieldCatalog[i].fieldName);
        if (access.property == nullptr) {
            ledger.disable_for_world();
            return GatewayStatus::preflightFailed;
        }
        // 条件恢复：仅当当前值仍等于本功能写入的覆盖值才写回原值。激活期间被
        // 游戏或其他 mod 改过的字段（当前值 != 覆盖值）视为本功能的恢复责任
        // 已消失，不得用陈旧快照覆盖更新后的值。
        const float current = access.property->GetPropertyValueInContainer(access.container);
        if (current != kFieldCatalog[i].overrideValue) {
            continue;
        }
        access.property->SetPropertyValueInContainer(access.container, (*originals)[i]);
        if (access.property->GetPropertyValueInContainer(access.container) != (*originals)[i]) {
            ledger.disable_for_world();
            return GatewayStatus::rollbackFailed;
        }
    }
    ledger.clear_records();
    return GatewayStatus::succeeded;
}

}  // namespace fishing_boost
