/**
 * @file capture_override_runtime.cpp
 * @brief 实现捕获覆盖 Hook 的精确预检、临时字段事务和对称恢复。
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <capture_override/capture_override_runtime.hpp>
#include <common/function_hook_registry.hpp>
#include <common/game_reflection.hpp>

namespace capture_override {
using namespace RC;
using namespace RC::Unreal;

namespace {

constexpr const TCHAR* kCharacterClassPath = STR("/Script/Pal.PalCharacter");
constexpr const TCHAR* kSphereClassPath = STR("/Script/Pal.PalSphereBodyBase");
constexpr const TCHAR* kStaticComponentClassPath =
    STR("/Script/Pal.PalStaticCharacterParameterComponent");
constexpr const TCHAR* kParameterComponentClassPath =
    STR("/Script/Pal.PalCharacterParameterComponent");
constexpr const TCHAR* kIndividualParameterClassPath =
    STR("/Script/Pal.PalIndividualCharacterParameter");
constexpr const TCHAR* kSpawnedTypeEnumPath = STR("/Script/Pal.EPalSpawnedCharacterType");
constexpr std::uint64_t kSpawnedCommon = 0;
constexpr float kForcedCaptureRate = 9999.0F;
constexpr std::size_t kMaximumNestedHookCalls = 16;

enum class ParameterKind : std::uint8_t { int32, object };

struct ParameterSpec {
    const TCHAR* name{};
    ParameterKind kind{ParameterKind::object};
    const TCHAR* objectClassPath{};
};

struct CaptureHookSpec {
    const TCHAR* path{};
    std::span<const ParameterSpec> parameters;
};

constexpr std::array kSphereSetupParameters{
    ParameterSpec{STR("TargetCharacter"), ParameterKind::object, kCharacterClassPath},
};
constexpr std::array kControllerSetupParameters{
    ParameterSpec{STR("Target"), ParameterKind::object, kSphereClassPath},
    ParameterSpec{STR("TargetCharacter"), ParameterKind::object, kCharacterClassPath},
};
constexpr std::array kControllerRpcParameters{
    ParameterSpec{STR("ID"), ParameterKind::int32, nullptr},
    ParameterSpec{STR("Target"), ParameterKind::object, kSphereClassPath},
    ParameterSpec{STR("TargetCharacter"), ParameterKind::object, kCharacterClassPath},
};
constexpr std::array kCaptureHookManifest{
    CaptureHookSpec{STR("/Script/Pal.PalSphereBodyBase:SetupInternal"), kSphereSetupParameters},
    CaptureHookSpec{STR("/Script/Pal.PalPlayerController:SetupInternalForSphere"),
                    kControllerSetupParameters},
    CaptureHookSpec{STR("/Script/Pal.PalPlayerController:SetupInternalForSphere_ToServer"),
                    kControllerRpcParameters},
    CaptureHookSpec{STR("/Script/Pal.PalPlayerController:SetupInternalForSphere_ToALL"),
                    kControllerRpcParameters},
};

struct BoolSetter {
    UFunction* function{};
    FBoolProperty* parameter{};
};

struct EnumSetter {
    UFunction* function{};
    FEnumProperty* parameter{};
};

struct BoolOverride {
    UObject* object{};
    FBoolProperty* property{};
    bool original{};
    bool desired{};
    bool changed{};
    std::optional<BoolSetter> setter;
};

struct FloatOverride {
    UObject* object{};
    FFloatProperty* property{};
    float original{};
    float desired{};
    bool changed{};
};

struct EnumOverride {
    UObject* object{};
    FEnumProperty* property{};
    EnumSetter setter;
    std::uint64_t original{};
    std::uint64_t desired{};
    bool changed{};
};

struct CaptureTransaction {
    /** @brief 6 个静态标志 + IsPal + bIsUncapturable + 强制模式的 bIsForceCapturable = 最多 9。 */
    std::array<BoolOverride, 9> bools{};
    std::size_t boolCount{};
    std::optional<FloatOverride> captureRate;
    std::optional<EnumOverride> spawnedType;
    /** @brief 目标原本不是帕鲁（人类 NPC）；用于诊断日志。 */
    bool nonPalTarget{};
};

struct ObjectPreparation {
    CapturePreparationStatus status{CapturePreparationStatus::unavailable};
    UObject* object{};
};

struct TransactionPreparation {
    CapturePreparationStatus status{CapturePreparationStatus::unavailable};
    CaptureTransaction transaction;
};

struct PendingHookCall {
    UFunction* function{};
    std::optional<CaptureTransaction> transaction;
};

enum class ApplyTransactionResult : std::uint8_t {
    success,
    failedAndRestored,
    rollbackFailed,
};

[[nodiscard]] auto find_class(const TCHAR* path) -> UClass* {
    return UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path);
}

[[nodiscard]] auto classify_object(UObject* object, const TCHAR* expectedClassPath)
    -> CapturePreparationStatus {
    if (!pal_game::is_valid(object)) {
        return CapturePreparationStatus::unavailable;
    }
    auto* const expectedClass = find_class(expectedClassPath);
    return expectedClass != nullptr && object->GetClassPrivate()->IsChildOf(expectedClass)
               ? CapturePreparationStatus::ready
               : CapturePreparationStatus::incompatible;
}

[[nodiscard]] auto matches_object_class(FObjectPropertyBase* property,
                                        const TCHAR* expectedClassPath) -> bool {
    auto* const expectedClass = find_class(expectedClassPath);
    return property != nullptr && expectedClass != nullptr &&
           property->GetPropertyClass().Get() == expectedClass;
}

[[nodiscard]] auto validate_hook_signature(UFunction* function, const CaptureHookSpec& spec)
    -> bool {
    if (function == nullptr ||
        !pal_game::has_exact_parameter_count(function, spec.parameters.size()) ||
        function->GetReturnProperty() != nullptr) {
        return false;
    }
    for (const auto& parameterSpec : spec.parameters) {
        auto* const property = function->FindProperty(FName(parameterSpec.name, FNAME_Find));
        if (!pal_game::is_input_parameter(property)) {
            return false;
        }
        if (parameterSpec.kind == ParameterKind::int32) {
            if (CastField<FIntProperty>(property) == nullptr) {
                return false;
            }
            continue;
        }
        if (!matches_object_class(CastField<FObjectPropertyBase>(property),
                                  parameterSpec.objectClassPath)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto read_target_character(UFunction* function, void* locals) -> ObjectPreparation {
    if (function == nullptr) {
        return {.status = CapturePreparationStatus::incompatible};
    }
    if (locals == nullptr) {
        return {.status = CapturePreparationStatus::unavailable};
    }
    auto* const property = CastField<FObjectPropertyBase>(
        function->FindProperty(FName(STR("TargetCharacter"), FNAME_Find)));
    if (!pal_game::is_input_parameter(property) ||
        !matches_object_class(property, kCharacterClassPath)) {
        return {.status = CapturePreparationStatus::incompatible};
    }
    auto* const character =
        property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(locals));
    const auto status = classify_object(character, kCharacterClassPath);
    return {.status = status,
            .object = status == CapturePreparationStatus::ready ? character : nullptr};
}

[[nodiscard]] auto read_object_property(UObject* object, const TCHAR* propertyName,
                                        const TCHAR* expectedClassPath) -> ObjectPreparation {
    if (!pal_game::is_valid(object)) {
        return {.status = CapturePreparationStatus::unavailable};
    }
    auto* const property =
        CastField<FObjectPropertyBase>(object->GetPropertyByNameInChain(propertyName));
    if (!matches_object_class(property, expectedClassPath)) {
        return {.status = CapturePreparationStatus::incompatible};
    }
    auto* const value =
        property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(object));
    const auto status = classify_object(value, expectedClassPath);
    return {.status = status,
            .object = status == CapturePreparationStatus::ready ? value : nullptr};
}

[[nodiscard]] auto call_object_getter(UObject* object, const TCHAR* functionName,
                                      const TCHAR* expectedClassPath) -> ObjectPreparation {
    if (!pal_game::is_valid(object)) {
        return {.status = CapturePreparationStatus::unavailable};
    }
    auto* const function = object->GetFunctionByNameInChain(functionName);
    auto* const result = function == nullptr
                             ? nullptr
                             : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_return_parameter(result) ||
        !matches_object_class(result, expectedClassPath)) {
        return {.status = CapturePreparationStatus::incompatible};
    }
    pal_game::FunctionParams params{function};
    object->ProcessEvent(function, params.data());
    if (!pal_game::is_valid(object)) {
        return {.status = CapturePreparationStatus::unavailable};
    }
    auto* const value =
        result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(params.data()));
    const auto status = classify_object(value, expectedClassPath);
    return {.status = status,
            .object = status == CapturePreparationStatus::ready ? value : nullptr};
}

[[nodiscard]] auto find_bool_setter(UObject* object, const TCHAR* functionName,
                                    const TCHAR* parameterName) -> std::optional<BoolSetter> {
    if (!pal_game::is_valid(object)) {
        return std::nullopt;
    }
    auto* const function = object->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return std::nullopt;
    }
    auto* const parameter =
        CastField<FBoolProperty>(function->FindProperty(FName(parameterName, FNAME_Find)));
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        function->GetReturnProperty() != nullptr || !pal_game::is_input_parameter(parameter)) {
        return std::nullopt;
    }
    return BoolSetter{.function = function, .parameter = parameter};
}

[[nodiscard]] auto find_enum_setter(UObject* object, const TCHAR* functionName,
                                    const TCHAR* parameterName) -> std::optional<EnumSetter> {
    if (!pal_game::is_valid(object)) {
        return std::nullopt;
    }
    auto* const function = object->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return std::nullopt;
    }
    auto* const parameter =
        CastField<FEnumProperty>(function->FindProperty(FName(parameterName, FNAME_Find)));
    auto* const expectedEnum =
        UObjectGlobals::StaticFindObject<UEnum*>(nullptr, nullptr, kSpawnedTypeEnumPath);
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        function->GetReturnProperty() != nullptr || !pal_game::is_input_parameter(parameter) ||
        expectedEnum == nullptr || parameter->GetEnum().Get() != expectedEnum ||
        CastField<FByteProperty>(parameter->GetUnderlyingProperty()) == nullptr) {
        return std::nullopt;
    }
    return EnumSetter{.function = function, .parameter = parameter};
}

[[nodiscard]] auto make_bool_override(UObject* object, const TCHAR* propertyName,
                                      const bool desired) -> std::optional<BoolOverride> {
    if (!pal_game::is_valid(object)) {
        return std::nullopt;
    }
    auto* const property = CastField<FBoolProperty>(object->GetPropertyByNameInChain(propertyName));
    if (property == nullptr) {
        return std::nullopt;
    }
    const bool original = property->GetPropertyValueInContainer(object);
    return BoolOverride{.object = object,
                        .property = property,
                        .original = original,
                        .desired = desired,
                        .changed = original != desired};
}

[[nodiscard]] auto make_float_override(UObject* object, const TCHAR* propertyName,
                                       const float desired) -> std::optional<FloatOverride> {
    if (!pal_game::is_valid(object)) {
        return std::nullopt;
    }
    auto* const property =
        CastField<FFloatProperty>(object->GetPropertyByNameInChain(propertyName));
    if (property == nullptr) {
        return std::nullopt;
    }
    const float original = property->GetPropertyValueInContainer(object);
    return FloatOverride{.object = object,
                         .property = property,
                         .original = original,
                         .desired = desired,
                         .changed = original != desired};
}

[[nodiscard]] auto make_enum_override(UObject* object, const TCHAR* propertyName,
                                      const EnumSetter& setter, const std::uint64_t desired)
    -> std::optional<EnumOverride> {
    if (!pal_game::is_valid(object)) {
        return std::nullopt;
    }
    auto* const property = CastField<FEnumProperty>(object->GetPropertyByNameInChain(propertyName));
    auto* const expectedEnum =
        UObjectGlobals::StaticFindObject<UEnum*>(nullptr, nullptr, kSpawnedTypeEnumPath);
    auto* const underlying =
        property == nullptr ? nullptr : CastField<FByteProperty>(property->GetUnderlyingProperty());
    if (property == nullptr || expectedEnum == nullptr ||
        property->GetEnum().Get() != expectedEnum || underlying == nullptr) {
        return std::nullopt;
    }
    const auto original =
        underlying->GetUnsignedIntPropertyValue(property->ContainerPtrToValuePtr<void>(object));
    return EnumOverride{.object = object,
                        .property = property,
                        .setter = setter,
                        .original = original,
                        .desired = desired,
                        .changed = original != desired};
}

[[nodiscard]] auto append_bool(CaptureTransaction& transaction, std::optional<BoolOverride> value)
    -> bool {
    if (!value.has_value() || transaction.boolCount >= transaction.bools.size()) {
        return false;
    }
    transaction.bools[transaction.boolCount++] = *value;
    return true;
}

[[nodiscard]] auto prepare_transaction(UObject* character, const bool forceHundredPercent)
    -> TransactionPreparation {
    if (!pal_game::is_valid(character)) {
        return {.status = CapturePreparationStatus::unavailable};
    }
    const auto staticComponentResult = read_object_property(
        character, STR("StaticCharacterParameterComponent"), kStaticComponentClassPath);
    if (staticComponentResult.status != CapturePreparationStatus::ready) {
        return {.status = staticComponentResult.status};
    }
    const auto parameterComponentResult = read_object_property(
        character, STR("CharacterParameterComponent"), kParameterComponentClassPath);
    if (parameterComponentResult.status != CapturePreparationStatus::ready) {
        return {.status = parameterComponentResult.status};
    }
    const auto individualParameterResult =
        call_object_getter(parameterComponentResult.object, STR("GetIndividualParameter"),
                           kIndividualParameterClassPath);
    if (individualParameterResult.status != CapturePreparationStatus::ready) {
        return {.status = individualParameterResult.status};
    }
    auto* const staticComponent = staticComponentResult.object;
    auto* const individualParameter = individualParameterResult.object;

    CaptureTransaction transaction;
    for (const auto* fieldName :
         {STR("IsUncapturable"), STR("IsBoss_Database"), STR("IsTowerBoss_Database"),
          STR("IsRaidBoss_Database"), STR("IsPredatorBoss_Database"), STR("IsRaidBoss_BP")}) {
        if (!append_bool(transaction, make_bool_override(staticComponent, fieldName, false))) {
            return {.status = pal_game::is_valid(staticComponent)
                                  ? CapturePreparationStatus::incompatible
                                  : CapturePreparationStatus::unavailable};
        }
    }

    // 人类 NPC（如商人）的 IsPal=false 是独立于不可捕获标志的捕获门控；对真帕鲁是无变化的
    // 空操作，仅在投球调用窗口内临时翻转为 true 并在 post-hook 恢复。
    const auto isPalOverride = make_bool_override(staticComponent, STR("IsPal"), true);
    if (!isPalOverride.has_value()) {
        return {.status = pal_game::is_valid(staticComponent)
                              ? CapturePreparationStatus::incompatible
                              : CapturePreparationStatus::unavailable};
    }
    transaction.nonPalTarget = !isPalOverride->original;
    if (!append_bool(transaction, isPalOverride)) {
        return {.status = CapturePreparationStatus::incompatible};
    }

    auto uncapturable = make_bool_override(individualParameter, STR("bIsUncapturable"), false);
    const auto uncapturableSetter =
        find_bool_setter(individualParameter, STR("SetUncapturable"), STR("bInUncapturable"));
    if (!uncapturable.has_value() || !uncapturableSetter.has_value()) {
        return {.status = pal_game::is_valid(individualParameter)
                              ? CapturePreparationStatus::incompatible
                              : CapturePreparationStatus::unavailable};
    }
    uncapturable->setter = uncapturableSetter;
    if (!append_bool(transaction, uncapturable)) {
        return {.status = CapturePreparationStatus::incompatible};
    }

    if (!forceHundredPercent) {
        return {.status = CapturePreparationStatus::ready, .transaction = transaction};
    }

    auto forceCapturable = make_bool_override(individualParameter, STR("bIsForceCapturable"), true);
    const auto forceSetter =
        find_bool_setter(individualParameter, STR("SetForceCapturable"), STR("bInForceCapturable"));
    transaction.captureRate =
        make_float_override(staticComponent, STR("CaptureSuccessRate"), kForcedCaptureRate);
    const auto spawnedSetter =
        find_enum_setter(staticComponent, STR("SetSpawnedCharacterType"), STR("SpawnedType"));
    if (!forceCapturable.has_value() || !forceSetter.has_value() ||
        !transaction.captureRate.has_value() || !spawnedSetter.has_value()) {
        return {.status =
                    pal_game::is_valid(staticComponent) && pal_game::is_valid(individualParameter)
                        ? CapturePreparationStatus::incompatible
                        : CapturePreparationStatus::unavailable};
    }
    forceCapturable->setter = forceSetter;
    if (!append_bool(transaction, forceCapturable)) {
        return {.status = CapturePreparationStatus::incompatible};
    }
    transaction.spawnedType = make_enum_override(staticComponent, STR("SpawnedCharacterType"),
                                                 *spawnedSetter, kSpawnedCommon);
    if (!transaction.spawnedType.has_value()) {
        return {.status = pal_game::is_valid(staticComponent)
                              ? CapturePreparationStatus::incompatible
                              : CapturePreparationStatus::unavailable};
    }
    return {.status = CapturePreparationStatus::ready, .transaction = transaction};
}

[[nodiscard]] auto call_bool_setter(UObject* object, const BoolSetter& setter, const bool value)
    -> bool {
    if (!pal_game::is_valid(object)) {
        return false;
    }
    pal_game::FunctionParams params{setter.function};
    setter.parameter->SetPropertyValueInContainer(params.data(), value);
    object->ProcessEvent(setter.function, params.data());
    return pal_game::is_valid(object);
}

[[nodiscard]] auto call_enum_setter(UObject* object, const EnumSetter& setter,
                                    const std::uint64_t value) -> bool {
    if (!pal_game::is_valid(object)) {
        return false;
    }
    pal_game::FunctionParams params{setter.function};
    auto* const underlying = setter.parameter->GetUnderlyingProperty();
    underlying->SetIntPropertyValue(setter.parameter->ContainerPtrToValuePtr<void>(params.data()),
                                    static_cast<std::int64_t>(value));
    object->ProcessEvent(setter.function, params.data());
    return pal_game::is_valid(object);
}

[[nodiscard]] auto enum_value(const EnumOverride& value) -> std::optional<std::uint64_t> {
    if (!pal_game::is_valid(value.object)) {
        return std::nullopt;
    }
    auto* const underlying = value.property->GetUnderlyingProperty();
    return underlying->GetUnsignedIntPropertyValue(
        value.property->ContainerPtrToValuePtr<void>(value.object));
}

[[nodiscard]] auto verify_desired(const CaptureTransaction& transaction) -> bool {
    for (std::size_t index = 0; index < transaction.boolCount; ++index) {
        const auto& value = transaction.bools[index];
        if (!pal_game::is_valid(value.object) ||
            value.property->GetPropertyValueInContainer(value.object) != value.desired) {
            return false;
        }
    }
    if (transaction.captureRate.has_value()) {
        const auto& value = *transaction.captureRate;
        if (!pal_game::is_valid(value.object) ||
            value.property->GetPropertyValueInContainer(value.object) != value.desired) {
            return false;
        }
    }
    if (transaction.spawnedType.has_value()) {
        const auto current = enum_value(*transaction.spawnedType);
        if (!current.has_value() || *current != transaction.spawnedType->desired) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto verify_original(const CaptureTransaction& transaction) -> bool {
    for (std::size_t index = 0; index < transaction.boolCount; ++index) {
        const auto& value = transaction.bools[index];
        if (!pal_game::is_valid(value.object) ||
            value.property->GetPropertyValueInContainer(value.object) != value.original) {
            return false;
        }
    }
    if (transaction.captureRate.has_value()) {
        const auto& value = *transaction.captureRate;
        if (!pal_game::is_valid(value.object) ||
            value.property->GetPropertyValueInContainer(value.object) != value.original) {
            return false;
        }
    }
    if (transaction.spawnedType.has_value()) {
        const auto current = enum_value(*transaction.spawnedType);
        if (!current.has_value() || *current != transaction.spawnedType->original) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto restore_transaction(CaptureTransaction& transaction) -> bool {
    bool restored = true;
    if (transaction.spawnedType.has_value() && transaction.spawnedType->changed) {
        auto& value = *transaction.spawnedType;
        restored = call_enum_setter(value.object, value.setter, value.original) && restored;
    }
    if (transaction.captureRate.has_value() && transaction.captureRate->changed &&
        pal_game::is_valid(transaction.captureRate->object)) {
        transaction.captureRate->property->SetPropertyValueInContainer(
            transaction.captureRate->object, transaction.captureRate->original);
    }
    for (std::size_t index = transaction.boolCount; index > 0; --index) {
        auto& value = transaction.bools[index - 1];
        if (!value.changed || !pal_game::is_valid(value.object)) {
            continue;
        }
        if (value.setter.has_value()) {
            restored = call_bool_setter(value.object, *value.setter, value.original) && restored;
        } else {
            value.property->SetPropertyValueInContainer(value.object, value.original);
        }
    }
    return restored && verify_original(transaction);
}

[[nodiscard]] auto failed_apply_result(CaptureTransaction& transaction) -> ApplyTransactionResult {
    return restore_transaction(transaction) ? ApplyTransactionResult::failedAndRestored
                                            : ApplyTransactionResult::rollbackFailed;
}

[[nodiscard]] auto apply_transaction(CaptureTransaction& transaction) -> ApplyTransactionResult {
    for (std::size_t index = 0; index < transaction.boolCount; ++index) {
        auto& value = transaction.bools[index];
        if (!value.changed) {
            continue;
        }
        if (value.setter.has_value()) {
            if (!call_bool_setter(value.object, *value.setter, value.desired)) {
                return failed_apply_result(transaction);
            }
        } else {
            value.property->SetPropertyValueInContainer(value.object, value.desired);
        }
    }
    if (transaction.captureRate.has_value() && transaction.captureRate->changed) {
        auto& value = *transaction.captureRate;
        if (!pal_game::is_valid(value.object)) {
            return failed_apply_result(transaction);
        }
        value.property->SetPropertyValueInContainer(value.object, value.desired);
    }
    if (transaction.spawnedType.has_value() && transaction.spawnedType->changed &&
        !call_enum_setter(transaction.spawnedType->object, transaction.spawnedType->setter,
                          transaction.spawnedType->desired)) {
        return failed_apply_result(transaction);
    }
    if (verify_desired(transaction)) {
        return ApplyTransactionResult::success;
    }
    return failed_apply_result(transaction);
}

}  // namespace

struct CaptureOverrideRuntime::Impl {
    Impl() : hookRegistry{STR("CaptureOverrideScript")} {}

    pal_game::FunctionHookRegistry hookRegistry;
    std::array<PendingHookCall, kMaximumNestedHookCalls> calls{};
    std::size_t callDepth{};
    std::size_t ignoredDepth{};
};

CaptureOverrideRuntime::CaptureOverrideRuntime() : impl_{std::make_unique<Impl>()} {}

CaptureOverrideRuntime::~CaptureOverrideRuntime() = default;

auto CaptureOverrideRuntime::set_config(const CaptureOverrideConfig& config) -> void {
    state_.set_config(config);
    reconcile_hooks();
}

auto CaptureOverrideRuntime::on_world_begin() -> void {
    unregister_hooks();
    state_.begin_world();
    reconcile_hooks();
}

auto CaptureOverrideRuntime::on_world_end() -> void {
    unregister_hooks();
    state_.end_world();
}

auto CaptureOverrideRuntime::tick() -> void {
    reconcile_hooks();
}

auto CaptureOverrideRuntime::shutdown() -> void {
    unregister_hooks();
    state_.end_world();
}

auto CaptureOverrideRuntime::phase() const noexcept -> CaptureRuntimePhase {
    return state_.phase();
}

auto CaptureOverrideRuntime::reconcile_hooks() -> void {
    if (state_.phase() == CaptureRuntimePhase::safetyDisabled) {
        if (impl_->callDepth == 0 && impl_->ignoredDepth == 0 && !impl_->hookRegistry.empty()) {
            unregister_hooks();
        }
        return;
    }
    if (state_.should_remove_hooks()) {
        unregister_hooks();
    } else if (state_.should_register_hooks()) {
        ensure_hooks_registered();
    }
}

auto CaptureOverrideRuntime::ensure_hooks_registered() -> void {
    if (!state_.should_register_hooks() || !impl_->hookRegistry.empty()) {
        return;
    }

    std::array<UFunction*, kCaptureHookManifest.size()> functions{};
    for (std::size_t index = 0; index < kCaptureHookManifest.size(); ++index) {
        const auto& spec = kCaptureHookManifest[index];
        functions[index] =
            UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, spec.path);
        if (!validate_hook_signature(functions[index], spec)) {
            state_.disable_for_world();
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: capture hook signature unavailable: {}\n"), spec.path);
            return;
        }
    }

    for (std::size_t index = 0; index < functions.size(); ++index) {
        auto* const function = functions[index];
        auto preCallback = [this, function](UnrealScriptFunctionCallableContext& context, void*) {
            if (impl_->callDepth >= impl_->calls.size()) {
                ++impl_->ignoredDepth;
                state_.disable_for_world();
                return;
            }
            auto& pending = impl_->calls[impl_->callDepth++];
            pending = {.function = function};
            if (state_.phase() != CaptureRuntimePhase::hooksRegistered) {
                return;
            }
            const auto target = read_target_character(function, context.TheStack.Locals());
            state_.observe_preparation_status(target.status);
            if (target.status != CapturePreparationStatus::ready) {
                return;
            }
            auto preparation =
                prepare_transaction(target.object, state_.config().forceHundredPercent);
            state_.observe_preparation_status(preparation.status);
            if (preparation.status != CapturePreparationStatus::ready) {
                return;
            }
            const auto applyResult = apply_transaction(preparation.transaction);
            if (applyResult == ApplyTransactionResult::success) {
                // 先入账再诊断：日志在极端情况下抛出时，恢复责任已经就位。
                pending.transaction = preparation.transaction;
                if (pending.transaction->nonPalTarget) {
                    Output::send<LogLevel::Verbose>(
                        STR("PalworldEditor: capture override applied to a non-Pal target "
                            "(IsPal temporarily true).\n"));
                }
                return;
            }
            if (applyResult == ApplyTransactionResult::rollbackFailed) {
                pending.transaction = preparation.transaction;
            }
            state_.disable_for_world();
        };
        auto postCallback = [this, function](UnrealScriptFunctionCallableContext&, void*) {
            if (impl_->ignoredDepth > 0) {
                --impl_->ignoredDepth;
                return;
            }
            if (impl_->callDepth == 0) {
                state_.disable_for_world();
                return;
            }
            auto& pending = impl_->calls[impl_->callDepth - 1];
            if (pending.function != function) {
                state_.disable_for_world();
                return;
            }
            if (pending.transaction.has_value() && !restore_transaction(*pending.transaction)) {
                state_.disable_for_world();
            }
            pending = {};
            --impl_->callDepth;
        };
        if (!impl_->hookRegistry.register_hook(function, std::move(preCallback),
                                               std::move(postCallback))) {
            impl_->hookRegistry.unregister_all();
            state_.disable_for_world();
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: capture hook registration failed: {}\n"),
                kCaptureHookManifest[index].path);
            return;
        }
    }
    state_.hooks_registered();
}

auto CaptureOverrideRuntime::unregister_hooks() -> void {
    for (std::size_t index = impl_->callDepth; index > 0; --index) {
        auto& pending = impl_->calls[index - 1];
        try {
            if (pending.transaction.has_value() && !restore_transaction(*pending.transaction)) {
                state_.disable_for_world();
            }
        } catch (...) {
            state_.disable_for_world();
        }
        pending = {};
    }
    impl_->callDepth = 0;
    impl_->ignoredDepth = 0;
    impl_->hookRegistry.unregister_all();
    state_.hooks_removed();
}

}  // namespace capture_override
