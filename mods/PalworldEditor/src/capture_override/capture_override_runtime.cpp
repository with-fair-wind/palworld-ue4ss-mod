/**
 * @file capture_override_runtime.cpp
 * @brief 实现 4 个投球 pre-hook 的按需注册/注销，以及回调内实时写入捕获标志。
 * @details 所有反射操作仅在游戏线程的 hook 回调内执行；不缓存 UObject 指针。从参数
 *          缓冲区按属性名读取 TargetCharacter，不手写固定参数布局（AGENTS.md 强制规则）。
 */
#include <capture_override/capture_override_runtime.hpp>

#include <array>
#include <string_view>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/game_reflection.hpp>

namespace capture_override {
using namespace RC;
using namespace RC::Unreal;

namespace {

/** @brief 4 个投球入口的完整 UFunction 路径。 */
constexpr std::array<std::string_view, 4> kCaptureHookPaths{
    "/Script/Pal.PalSphereBodyBase:SetupInternal",
    "/Script/Pal.PalPlayerController:SetupInternalForSphere",
    "/Script/Pal.PalPlayerController:SetupInternalForSphere_ToServer",
    "/Script/Pal.PalPlayerController:SetupInternalForSphere_ToALL",
};

/** @brief 所有 hook 的最后一个参数名，类型为 APalCharacter*。 */
constexpr const TCHAR* kTargetCharacterParam = STR("TargetCharacter");

/**
 * @brief 从 hook 参数缓冲区按属性名读取 TargetCharacter 对象指针。
 * @details 不手写固定 struct；用 FObjectPropertyBase 按名定位参数后取值。
 *         签名不匹配时返回 nullptr（fail-closed）。
 */
[[nodiscard]] auto read_target_character(UFunction* function, void* locals) -> UObject* {
    if (function == nullptr || locals == nullptr) {
        return nullptr;
    }
    auto* property = CastField<FObjectPropertyBase>(
        function->FindProperty(FName(kTargetCharacterParam, FNAME_Find)));
    if (property == nullptr) {
        return nullptr;
    }
    auto* character =
        property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(locals));
    return pal_game::is_valid(character) ? character : nullptr;
}

/**
 * @brief 读取角色上的 StaticCharacterParameterComponent 对象指针。
 * @details 该组件是 APalCharacter 的直接对象指针字段。
 */
[[nodiscard]] auto read_static_component(UObject* character) -> UObject* {
    auto* property =
        CastField<FObjectPropertyBase>(character->GetPropertyByNameInChain(
            STR("StaticCharacterParameterComponent")));
    if (property == nullptr) {
        return nullptr;
    }
    auto* component =
        property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(character));
    return pal_game::is_valid(component) ? component : nullptr;
}

/**
 * @brief 读取角色上的 CharacterParameterComponent 并通过 GetIndividualParameter 取个体参数。
 */
[[nodiscard]] auto read_individual_parameter(UObject* character) -> UObject* {
    auto* paramCompProp =
        CastField<FObjectPropertyBase>(character->GetPropertyByNameInChain(
            STR("CharacterParameterComponent")));
    auto* parameterComponent =
        paramCompProp == nullptr
            ? nullptr
            : paramCompProp->GetObjectPropertyValue(
                  paramCompProp->ContainerPtrToValuePtr<void>(character));
    if (!pal_game::is_valid(parameterComponent)) {
        return nullptr;
    }
    auto* individual = pal_game::invoke<UObject*>(parameterComponent, STR("GetIndividualParameter"))
                           .value_or(nullptr);
    return pal_game::is_valid(individual) ? individual : nullptr;
}

/** @brief 安全写入一个 bool 属性；属性缺失或类型不符时静默跳过。 */
auto set_bool(UObject* object, const TCHAR* fieldName, const bool value) -> void {
    auto* property =
        CastField<FBoolProperty>(object->GetPropertyByNameInChain(fieldName));
    if (property == nullptr) {
        return;
    }
    property->SetPropertyValueInContainer(object, value);
}

/** @brief 安全写入一个 float 属性。 */
auto set_float(UObject* object, const TCHAR* fieldName, const float value) -> void {
    auto* property =
        CastField<FFloatProperty>(object->GetPropertyByNameInChain(fieldName));
    if (property == nullptr) {
        return;
    }
    property->SetPropertyValueInContainer(object, value);
}

/** @brief 调用零参数 void setter（如 SetSpawnedCharacterType）；签名不符时跳过。 */
auto call_setter(UObject* object, const TCHAR* methodName, const std::int64_t enumValue) -> void {
    auto* function = object->GetFunctionByNameInChain(methodName);
    if (function == nullptr || !pal_game::has_exact_parameter_count(function, 1)) {
        return;
    }
    auto* param = function->FindProperty(FName(STR("SpawnedType"), FNAME_Find));
    if (param == nullptr || !pal_game::is_input_parameter(param)) {
        return;
    }
    auto* enumProperty = CastField<FEnumProperty>(param);
    if (enumProperty == nullptr) {
        return;
    }
    pal_game::FunctionParams params{function};
    enumProperty->GetUnderlyingProperty()->SetIntPropertyValue(
        enumProperty->ContainerPtrToValuePtr<void>(params.data()), enumValue);
    object->ProcessEvent(function, params.data());
}

/**
 * @brief 在目标帕鲁上写入捕获标志，使其可通过捕获判定。
 * @param[in] character 目标帕鲁（非拥有，仅当次回调有效）。
 * @param[in] forceHundredPercent 额外强制接近 100% 成功率与强制可捕获。
 */
auto normalize_capture_target(UObject* character, const bool forceHundredPercent) -> void {
    auto* staticComponent = read_static_component(character);
    if (staticComponent != nullptr) {
        set_bool(staticComponent, STR("IsUncapturable"), false);
        set_bool(staticComponent, STR("IsBoss_Database"), false);
        set_bool(staticComponent, STR("IsTowerBoss_Database"), false);
        set_bool(staticComponent, STR("IsRaidBoss_Database"), false);
        set_bool(staticComponent, STR("IsPredatorBoss_Database"), false);
        set_bool(staticComponent, STR("IsRaidBoss_BP"), false);
        if (forceHundredPercent) {
            set_float(staticComponent, STR("CaptureSuccessRate"), 9999.0F);
            call_setter(staticComponent, STR("SetSpawnedCharacterType"), 0);
        }
    }

    auto* individualParameter = read_individual_parameter(character);
    if (individualParameter != nullptr) {
        set_bool(individualParameter, STR("bIsUncapturable"), false);
        if (forceHundredPercent) {
            set_bool(individualParameter, STR("bIsForceCapturable"), true);
        }
    }
}

}  // namespace

auto CaptureOverrideRuntime::set_config(const CaptureOverrideConfig& config) -> void {
    config_ = config;
    if (safetyDisabled_) {
        phase_ = CaptureRuntimePhase::safetyDisabled;
        return;
    }
    if (config_.enabled && !hooksRegistered_) {
        ensure_hooks_registered();
    } else if (!config_.enabled && hooksRegistered_) {
        unregister_hooks();
    }
}

auto CaptureOverrideRuntime::on_world_begin(const std::uint64_t /*generation*/) -> void {
    if (hooksRegistered_) {
        unregister_hooks();
    }
    safetyDisabled_ = false;
    config_ = {};
    phase_ = CaptureRuntimePhase::off;
}

auto CaptureOverrideRuntime::on_world_end() -> void {
    if (hooksRegistered_) {
        unregister_hooks();
    }
    phase_ = CaptureRuntimePhase::off;
}

auto CaptureOverrideRuntime::shutdown() -> void {
    if (hooksRegistered_) {
        unregister_hooks();
    }
}

auto CaptureOverrideRuntime::phase() const noexcept -> CaptureRuntimePhase {
    return phase_;
}

auto CaptureOverrideRuntime::ensure_hooks_registered() -> void {
    if (hooksRegistered_ || safetyDisabled_) {
        return;
    }

    hooks_.clear();
    hooks_.reserve(kCaptureHookPaths.size());
    for (const auto pathView : kCaptureHookPaths) {
        const std::wstring path{pathView.begin(), pathView.end()};
        auto* function =
            UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path.c_str());
        if (function == nullptr) {
            // 任一路径解析失败 → 不注册任何 hook，安全停用本世界。
            hooks_.clear();
            safetyDisabled_ = true;
            phase_ = CaptureRuntimePhase::safetyDisabled;
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: capture hook path unresolved: {}\n"), path.c_str());
            return;
        }
        const auto hookId = function->RegisterPreHook(
            [this, function](UnrealScriptFunctionCallableContext& context, void*) {
                if (!config_.enabled) {
                    return;
                }
                auto* character = read_target_character(function, context.TheStack.Locals());
                if (character != nullptr) {
                    normalize_capture_target(character, config_.forceHundredPercent);
                }
            });
        if (hookId < 0) {
            hooks_.clear();
            safetyDisabled_ = true;
            phase_ = CaptureRuntimePhase::safetyDisabled;
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: capture hook registration failed: {}\n"), path.c_str());
            return;
        }
        hooks_.push_back({.function = function, .hookId = hookId});
    }

    hooksRegistered_ = true;
    phase_ = CaptureRuntimePhase::hooksRegistered;
}

auto CaptureOverrideRuntime::unregister_hooks() -> void {
    for (auto hook = hooks_.rbegin(); hook != hooks_.rend(); ++hook) {
        if (hook->function != nullptr && hook->hookId >= 0) {
            static_cast<void>(hook->function->UnregisterHook(hook->hookId));
        }
    }
    hooks_.clear();
    hooksRegistered_ = false;
    phase_ = CaptureRuntimePhase::off;
}

CaptureOverrideRuntime::~CaptureOverrideRuntime() {
    // 析构不得访问 Unreal；hook 应在 shutdown/on_world_end 中先注销。
    hooks_.clear();
    hooksRegistered_ = false;
}

}  // namespace capture_override
