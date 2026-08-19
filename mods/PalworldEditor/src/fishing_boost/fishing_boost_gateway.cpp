/**
 * @file fishing_boost_gateway.cpp
 * @brief 实现 UPalFishingSystem.CatchBattleParameter 的读写与可逆恢复。
 */
#include <fishing_boost/fishing_boost_gateway.hpp>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/game_reflection.hpp>
#include <common/text_encoding.hpp>

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
    return {.property = floatProp,
            .container = structProp->ContainerPtrToValuePtr<void>(system)};
}

}  // namespace

auto apply(Ledger& ledger) -> GatewayStatus {
    auto* const system = resolve_system();
    if (system == nullptr) {
        return GatewayStatus::targetUnavailable;
    }

    std::array<float, kFieldCount> originals{};
    for (std::size_t i{}; i < kFieldCount; ++i) {
        const auto access = find_field(system, kFieldCatalog[i].fieldName);
        if (access.property == nullptr) {
            return GatewayStatus::preflightFailed;
        }
        originals[i] = access.property->GetPropertyValueInContainer(access.container);
        access.property->SetPropertyValueInContainer(access.container,
                                                     kFieldCatalog[i].overrideValue);
        if (access.property->GetPropertyValueInContainer(access.container) !=
            kFieldCatalog[i].overrideValue) {
            // 回滚已写入的部分
            for (std::size_t j{}; j <= i; ++j) {
                const auto rollback = find_field(system, kFieldCatalog[j].fieldName);
                if (rollback.property != nullptr) {
                    rollback.property->SetPropertyValueInContainer(rollback.container, originals[j]);
                }
            }
            return GatewayStatus::verificationFailed;
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
