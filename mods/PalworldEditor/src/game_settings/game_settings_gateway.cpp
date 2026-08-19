/**
 * @file game_settings_gateway.cpp
 * @brief 实现 UPalGameSetting 参数的读取、写入验证与可逆恢复。
 */
#include <game_settings/game_settings_gateway.hpp>

#include <vector>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/game_reflection.hpp>
#include <common/text_encoding.hpp>

namespace game_settings {
using namespace RC;
using namespace RC::Unreal;

namespace {

/** @brief 解析 UPalGameSetting 单例；不可用返回空。 */
[[nodiscard]] auto resolve_game_setting() -> UObject* {
    auto* const utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* const function =
        utility == nullptr ? nullptr : utility->GetFunctionByNameInChain(STR("GetGameSetting"));
    auto* const input =
        function == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(
                  function->FindProperty(FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const output =
        function == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(
                  function->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
    if (!pal_game::is_valid(utility) || !pal_game::has_exact_parameter_count(function, 2) ||
        !pal_game::is_input_parameter(input) || !pal_game::is_return_parameter(output)) {
        return nullptr;
    }
    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    if (!pal_game::is_valid(worldContext)) {
        return nullptr;
    }
    pal_game::FunctionParams params{function};
    input->SetObjectPropertyValue(input->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    utility->ProcessEvent(function, params.data());
    auto* const setting =
        output->GetObjectPropertyValue(output->ContainerPtrToValuePtr<void>(params.data()));
    return pal_game::is_valid(setting) ? setting : nullptr;
}

/** @brief 按规格找到对应类型的 FProperty；不匹配返回空。 */
[[nodiscard]] auto find_property(UObject* setting, const OverrideSpec& spec) -> FProperty* {
    const auto wideName = text_encoding::widen_ascii(spec.propertyName);
    auto* const raw = setting->GetPropertyByNameInChain(wideName.c_str());
    if (raw == nullptr) {
        return nullptr;
    }
    if (std::holds_alternative<std::int32_t>(spec.defaultValue)) {
        return CastField<FIntProperty>(raw);
    }
    return CastField<FFloatProperty>(raw);
}

/** @brief 读当前值。 */
[[nodiscard]] auto read_value(FProperty* property, UObject* setting) -> OverrideValue {
    if (auto* const intProp = CastField<FIntProperty>(property)) {
        return intProp->GetPropertyValueInContainer(setting);
    }
    if (auto* const floatProp = CastField<FFloatProperty>(property)) {
        return floatProp->GetPropertyValueInContainer(setting);
    }
    return std::int32_t{0};
}

/** @brief 写入值并返回是否成功。 */
auto write_value(FProperty* property, UObject* setting, const OverrideValue& value) -> bool {
    if (auto* const intProp = CastField<FIntProperty>(property)) {
        if (const auto* const v = std::get_if<std::int32_t>(&value)) {
            intProp->SetPropertyValueInContainer(setting, *v);
            return true;
        }
    }
    if (auto* const floatProp = CastField<FFloatProperty>(property)) {
        if (const auto* const v = std::get_if<float>(&value)) {
            floatProp->SetPropertyValueInContainer(setting, *v);
            return true;
        }
    }
    return false;
}

/** @brief 值是否相等（类型+值匹配）。 */
[[nodiscard]] auto values_equal(const OverrideValue& a, const OverrideValue& b) -> bool {
    if (a.index() != b.index()) {
        return false;
    }
    if (const auto* const ia = std::get_if<std::int32_t>(&a)) {
        return *ia == *std::get_if<std::int32_t>(&b);
    }
    const auto* const fa = std::get_if<float>(&a);
    return fa != nullptr && *fa == *std::get_if<float>(&b);
}

}  // namespace

auto apply_overrides(OverrideLedger& ledger) -> GatewayStatus {
    const auto pending = ledger.pending_indices();
    if (pending.empty()) {
        return GatewayStatus::succeeded;
    }
    auto* const setting = resolve_game_setting();
    if (setting == nullptr) {
        return GatewayStatus::targetUnavailable;
    }

    for (const auto index : pending) {
        const auto& spec = kOverrideCatalog[index];
        auto* const property = find_property(setting, spec);
        if (property == nullptr) {
            ledger.disable_for_world();
            return GatewayStatus::preflightFailed;
        }
        const OverrideValue original = read_value(property, setting);
        const OverrideValue& desired = *ledger.desired(index);
        if (values_equal(original, desired)) {
            ledger.record_applied(index, original);
            continue;
        }
        if (!write_value(property, setting, desired) ||
            !values_equal(read_value(property, setting), desired)) {
            // 回滚
            static_cast<void>(write_value(property, setting, original));
            if (!values_equal(read_value(property, setting), original)) {
                ledger.disable_for_world();
                return GatewayStatus::rollbackFailed;
            }
            return GatewayStatus::verificationFailed;
        }
        ledger.record_applied(index, original);
    }
    return GatewayStatus::succeeded;
}

auto restore_overrides(OverrideLedger& ledger) -> GatewayStatus {
    auto toRestore = ledger.active_indices();
    if (toRestore.empty()) {
        return GatewayStatus::succeeded;
    }
    auto* const setting = resolve_game_setting();
    if (setting == nullptr) {
        // GameSetting 实例已随世界销毁；新世界用原生值，无需恢复。
        ledger.clear_all_records();
        return GatewayStatus::succeeded;
    }

    for (const auto index : toRestore) {
        const auto& spec = kOverrideCatalog[index];
        auto* const property = find_property(setting, spec);
        const auto original = ledger.original(index);
        if (property == nullptr || !original.has_value()) {
            ledger.clear_record(index);
            continue;
        }
        const OverrideValue current = read_value(property, setting);
        if (values_equal(current, *original)) {
            ledger.clear_record(index);
            continue;
        }
        if (!write_value(property, setting, *original) ||
            !values_equal(read_value(property, setting), *original)) {
            ledger.disable_for_world();
            return GatewayStatus::rollbackFailed;
        }
        ledger.clear_record(index);
    }
    return GatewayStatus::succeeded;
}

}  // namespace game_settings
