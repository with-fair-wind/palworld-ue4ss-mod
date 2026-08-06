/**
 * @file remote_palbox_runtime.cpp
 * @brief 远程终端运行时实现：WinAPI 按键轮询、反射门控、基地解析与原生 HUD Push。
 * @details 所有反射调用在游戏线程一次性执行；跨帧只保存纯值（配置、计数、widget 路径字符串）。
 *          结构故障（HUD 服务/Push/参数工厂/基地管理器任一不可用）→ 本世界代次内停用。
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <chrono>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <common/game_reflection.hpp>
#include <common/text_encoding.hpp>
#include <pal_remote_palbox/remote_palbox_runtime.hpp>
#include <windows.h>

using namespace RC;
using namespace RC::Unreal;

namespace pal_remote_palbox {
namespace {

/** @brief 连续触发超时的次数上限；达到后停用域。 */
inline constexpr std::uint64_t kMaxConsecutiveTimeouts = 5;

/** @brief 单次触发允许的软耗时上限；超过仅记录日志。 */
inline constexpr auto kTriggerTimeBudget = std::chrono::milliseconds(2);

/** @brief PalBox 用户控件的生成类名（WBP_PalBox 蓝图资产）。 */
inline constexpr const wchar_t* kPalBoxWidgetClassName = L"WBP_PalBox_C";

/** @brief 世界内对象类名 → 单例实例查找。 */
[[nodiscard]] auto find_singleton(const wchar_t* className) -> UObject* {
    return UObjectGlobals::FindFirstOf(className);
}

/** @brief 本地玩家控制器（镜像资源分享的 PalUtility 调用模式）。 */
[[nodiscard]] auto local_player_controller(UObject* worldContext) -> UObject* {
    auto* utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* function = utility == nullptr
                         ? nullptr
                         : utility->GetFunctionByNameInChain(STR("GetLocalPalPlayerController"));
    auto* input = function == nullptr ? nullptr
                                      : CastField<FObjectPropertyBase>(function->FindProperty(
                                            FName(STR("WorldContextObject"), FNAME_Find)));
    auto* output = function == nullptr ? nullptr
                                       : CastField<FObjectPropertyBase>(function->FindProperty(
                                             FName(STR("ReturnValue"), FNAME_Find)));
    if (utility == nullptr || function == nullptr || input == nullptr || output == nullptr) {
        return nullptr;
    }
    pal_game::FunctionParams params{function};
    input->SetObjectPropertyValue(input->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    utility->ProcessEvent(function, params.data());
    auto* const controller =
        output->GetObjectPropertyValue(output->ContainerPtrToValuePtr<void>(params.data()));
    return pal_game::is_valid(controller) ? controller : nullptr;
}

/** @brief 无参 bool 返回的 UFunction 调用；不可用时返回 nullopt。 */
[[nodiscard]] auto call_bool(UObject* target, const wchar_t* functionName) -> std::optional<bool> {
    auto* function =
        pal_game::is_valid(target) ? target->GetFunctionByNameInChain(functionName) : nullptr;
    if (function == nullptr || function->GetParmsSize() != 0) {
        return std::nullopt;
    }
    pal_game::FunctionParams params{function};
    auto* const returnProperty = CastField<FBoolProperty>(function->GetReturnProperty());
    if (returnProperty == nullptr) {
        return std::nullopt;
    }
    target->ProcessEvent(function, params.data());
    return returnProperty->GetPropertyValueInContainer(params.data());
}

/** @brief 从 PalBaseCampManager 枚举本地玩家的基地 ID（镜像资源分享 read_base_ids）。 */
[[nodiscard]] auto read_base_ids(UObject* manager, std::vector<FGuid>& output) -> bool {
    output.clear();
    auto* function =
        manager == nullptr ? nullptr : manager->GetFunctionByNameInChain(STR("GetBaseCampIds"));
    auto* arrayProperty =
        function == nullptr
            ? nullptr
            : CastField<FArrayProperty>(function->FindProperty(FName(STR("OutIds"), FNAME_Find)));
    auto* guidProperty =
        arrayProperty == nullptr ? nullptr : CastField<FStructProperty>(arrayProperty->GetInner());
    if (function == nullptr || arrayProperty == nullptr || guidProperty == nullptr) {
        return false;
    }

    std::vector<std::byte> params(static_cast<std::size_t>(function->GetParmsSize()));
    function->InitializeStruct(params.data());
    struct ParamsGuard {
        UFunction* function{};
        void* params{};
        ~ParamsGuard() {
            function->DestroyStruct(params);
        }
    } guard{.function = function, .params = params.data()};

    manager->ProcessEvent(function, params.data());
    FScriptArrayHelper_InContainer values(arrayProperty, params.data());
    output.reserve(static_cast<std::size_t>(std::max(values.Num(), 0)));
    for (int32 index{}; index < values.Num(); ++index) {
        FGuid id{};
        guidProperty->CopyCompleteValue(&id, values.GetRawPtr(index));
        output.push_back(id);
    }
    return true;
}

/** @brief 按基地 ID 取 PalBaseCampModel（镜像资源分享 try_get_base_model）。 */
[[nodiscard]] auto try_get_base_model(UObject* manager, const FGuid& baseId, UObject*& model)
    -> bool {
    model = nullptr;
    auto* function =
        manager == nullptr ? nullptr : manager->GetFunctionByNameInChain(STR("TryGetModel"));
    auto* idProperty = function == nullptr ? nullptr
                                           : CastField<FStructProperty>(function->FindProperty(
                                                 FName(STR("BaseCampId"), FNAME_Find)));
    auto* modelProperty = function == nullptr
                              ? nullptr
                              : CastField<FObjectPropertyBase>(
                                    function->FindProperty(FName(STR("OutModel"), FNAME_Find)));
    auto* returnProperty =
        function == nullptr ? nullptr : CastField<FBoolProperty>(function->GetReturnProperty());
    if (function == nullptr || idProperty == nullptr || modelProperty == nullptr ||
        returnProperty == nullptr) {
        return false;
    }

    std::vector<std::byte> params(static_cast<std::size_t>(function->GetParmsSize()));
    function->InitializeStruct(params.data());
    struct ParamsGuard {
        UFunction* function{};
        void* params{};
        ~ParamsGuard() {
            function->DestroyStruct(params);
        }
    } guard{.function = function, .params = params.data()};

    idProperty->CopyCompleteValue(idProperty->ContainerPtrToValuePtr<void>(params.data()), &baseId);
    manager->ProcessEvent(function, params.data());
    model = modelProperty->GetObjectPropertyValue(
        modelProperty->ContainerPtrToValuePtr<void>(params.data()));
    return returnProperty->GetPropertyValueInContainer(params.data()) && model != nullptr;
}

/** @brief 从模型 getter 读取 FGuid 字段。 */
[[nodiscard]] auto read_guid(UObject* model, const wchar_t* getterName, FGuid& output) -> bool {
    auto* function =
        pal_game::is_valid(model) ? model->GetFunctionByNameInChain(getterName) : nullptr;
    auto* returnProperty =
        function == nullptr ? nullptr : CastField<FStructProperty>(function->GetReturnProperty());
    if (function == nullptr || returnProperty == nullptr) {
        return false;
    }
    pal_game::FunctionParams params{function};
    model->ProcessEvent(function, params.data());
    returnProperty->CopyCompleteValue(&output,
                                      returnProperty->ContainerPtrToValuePtr<void>(params.data()));
    return output.A != 0 || output.B != 0 || output.C != 0 || output.D != 0;
}

/** @brief 从模型读取 float 字段（AreaRange）。 */
[[nodiscard]] auto read_float_field(UObject* model, const wchar_t* fieldName, float& output)
    -> bool {
    auto* property =
        pal_game::is_valid(model) ? model->GetPropertyByNameInChain(fieldName) : nullptr;
    auto* floatProperty = CastField<FFloatProperty>(property);
    if (floatProperty == nullptr) {
        return false;
    }
    output = floatProperty->GetPropertyValueInContainer(model);
    return true;
}

/** @brief 读取对象位置（GetLocation/K2_GetActorLocation 均尝试）。 */
[[nodiscard]] auto read_location(UObject* object, FVector& output) -> bool {
    for (const wchar_t* functionName : {L"GetLocation", L"K2_GetActorLocation"}) {
        auto* function =
            pal_game::is_valid(object) ? object->GetFunctionByNameInChain(functionName) : nullptr;
        if (function == nullptr || function->GetParmsSize() != 0) {
            continue;
        }
        pal_game::FunctionParams params{function};
        auto* const returnProperty = CastField<FStructProperty>(function->GetReturnProperty());
        if (returnProperty == nullptr) {
            continue;
        }
        object->ProcessEvent(function, params.data());
        returnProperty->CopyCompleteValue(
            &output, returnProperty->ContainerPtrToValuePtr<void>(params.data()));
        return true;
    }
    return false;
}

/** @brief 玩家当前位置（Pawn → K2_GetActorLocation）。 */
[[nodiscard]] auto read_player_location(UObject* controller, FVector& output) -> bool {
    auto* const pawn = [&]() -> UObject* {
        auto* function = pal_game::is_valid(controller)
                             ? controller->GetFunctionByNameInChain(STR("GetPawn"))
                             : nullptr;
        if (function == nullptr) {
            return nullptr;
        }
        pal_game::FunctionParams params{function};
        auto* const returnProperty = CastField<FObjectPropertyBase>(function->GetReturnProperty());
        if (returnProperty == nullptr) {
            return nullptr;
        }
        controller->ProcessEvent(function, params.data());
        auto* const pawnValue = returnProperty->GetObjectPropertyValue(
            returnProperty->ContainerPtrToValuePtr<void>(params.data()));
        return pal_game::is_valid(pawnValue) ? pawnValue : nullptr;
    }();
    return pawn != nullptr && read_location(pawn, output);
}

/** @brief 玩家是否位于基地圈内；任一反射点不可用时 fail-closed 返回 false。 */
[[nodiscard]] auto player_inside_base(UObject* controller, UObject* model,
                                      UObject* mapObjectManager, const FGuid& ownerMapObjectId)
    -> bool {
    float areaRange{};
    FVector playerLocation{};
    if (!read_float_field(model, STR("AreaRange"), areaRange) || areaRange <= 0.0F ||
        !read_player_location(controller, playerLocation)) {
        return false;
    }
    auto* const concreteModel = [&]() -> UObject* {
        auto* function = pal_game::is_valid(mapObjectManager)
                             ? mapObjectManager->GetFunctionByNameInChain(STR("FindConcreteModel"))
                             : nullptr;
        auto* input = function == nullptr ? nullptr
                                          : CastField<FStructProperty>(function->FindProperty(
                                                FName(STR("InstanceId"), FNAME_Find)));
        if (function == nullptr || input == nullptr) {
            return nullptr;
        }
        pal_game::FunctionParams params{function};
        input->CopyCompleteValue(input->ContainerPtrToValuePtr<void>(params.data()),
                                 &ownerMapObjectId);
        mapObjectManager->ProcessEvent(function, params.data());
        auto* const returnProperty = CastField<FObjectPropertyBase>(function->GetReturnProperty());
        if (returnProperty == nullptr) {
            return nullptr;
        }
        auto* const result = returnProperty->GetObjectPropertyValue(
            returnProperty->ContainerPtrToValuePtr<void>(params.data()));
        return pal_game::is_valid(result) ? result : nullptr;
    }();
    if (concreteModel == nullptr) {
        return false;
    }
    FVector campCenter{};
    if (!read_location(concreteModel, campCenter)) {
        return false;
    }
    const double dx = (playerLocation.X() - campCenter.X());
    const double dy = (playerLocation.Y() - campCenter.Y());
    const auto range = static_cast<double>(areaRange);
    return (dx * dx) + (dy * dy) <= (range * range);
}

/** @brief 首次触发时定位 WBP_PalBox_C 的完整路径；结果缓存为纯字符串。 */
[[nodiscard]] auto resolve_widget_path(std::string& cachedPath, bool& resolved) -> bool {
    if (resolved) {
        return !cachedPath.empty();
    }
    resolved = true;
    std::wstring found;
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
        if (!found.empty()) {
            return LoopAction::Continue;
        }
        if (obj == nullptr || obj->GetClassPrivate() == nullptr) {
            return LoopAction::Continue;
        }
        if (obj->GetName() != kPalBoxWidgetClassName) {
            return LoopAction::Continue;
        }
        // 类对象的类名是 "Class"（UClass 自身的类）；排除同名实例。
        if (obj->GetClassPrivate()->GetName() != L"Class") {
            return LoopAction::Continue;
        }
        found = obj->GetPathName();
        return LoopAction::Continue;
    });
    cachedPath = text_encoding::to_utf8(found);
    if (cachedPath.empty()) {
        Output::send<LogLevel::Warning>(
            STR("PalworldEditor: WBP_PalBox_C not found; remote palbox unavailable\n"));
    } else {
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: resolved remote palbox widget path '{}'\n"),
            text_encoding::widen_ascii(cachedPath));
    }
    return !cachedPath.empty();
}

/** @brief 游戏是否处于前台（前台窗口属于本进程）。 */
[[nodiscard]] auto foreground_is_game() -> bool {
    auto* const foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD pid{};
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

/** @brief FGuid 是否全零。 */
[[nodiscard]] auto is_zero_guid(const FGuid& guid) -> bool {
    return guid.A == 0 && guid.B == 0 && guid.C == 0 && guid.D == 0;
}
}  // namespace

auto RemotePalboxRuntime::load_config(const std::string_view iniPath) -> void {
    iniPath_ = std::string{iniPath};
    std::string content;
    if (std::ifstream stream{std::string{iniPath}, std::ios::binary}; stream) {
        content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    config_ = parse_remote_palbox_config(content);
    trigger_.reset();
    note("配置已加载" + std::string{content.empty() ? "（使用默认值）" : ""});
}

auto RemotePalboxRuntime::set_config(const RemotePalboxConfig config) -> void {
    {
        const std::lock_guard lock(snapshotMutex_);
        config_ = config;
    }
    trigger_.reset();
    if (!iniPath_.empty()) {
        std::ofstream stream{std::string{iniPath_}, std::ios::binary | std::ios::trunc};
        if (stream) {
            stream << serialize_remote_palbox_config(config);
        } else {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: failed to write remote palbox config '{}'\n"),
                text_encoding::widen_ascii(iniPath_));
        }
    }
}

auto RemotePalboxRuntime::tick(const float deltaSeconds,
                               const skill_editor::WorldSessionState& session) -> void {
    static_cast<void>(deltaSeconds);
    const bool guiRequest = requestedOpen_.exchange(false);
    const bool pressed = (GetAsyncKeyState(config_.hotkeyVk) & 0x8000) != 0;
    const bool foreground = foreground_is_game();
    const bool triggered =
        guiRequest ||
        (foreground && pressed && trigger_.update(std::chrono::steady_clock::now(), pressed));
    if (!triggered) {
        return;
    }
    if (!session.can_access_unreal()) {
        note("世界尚未就绪");
        return;
    }
    static_cast<void>(execute_trigger());
}

auto RemotePalboxRuntime::request_open() -> void {
    requestedOpen_.store(true);
}

auto RemotePalboxRuntime::begin_world_transition() -> void {
    trigger_.reset();
    domainDisabled_ = false;
    domainProbed_ = false;
    requestedOpen_.store(false);
    consecutiveTimeoutCount_ = 0;
}

auto RemotePalboxRuntime::finish_world_transition() -> void {
    // widgetPath_ 字符串缓存跨世界保留；无需重置。
}

auto RemotePalboxRuntime::snapshot() const -> RemotePalboxSnapshot {
    const std::lock_guard lock(snapshotMutex_);
    return RemotePalboxSnapshot{
        .config = config_,
        .domainDisabled = domainDisabled_,
        .lastMessage = lastMessage_,
        .openCount = openCount_,
        .failCount = failCount_,
    };
}

auto RemotePalboxRuntime::execute_trigger() -> RemotePalboxTriggerResult {
    const auto startedAt = std::chrono::steady_clock::now();
    const auto finish = [this, startedAt](const RemotePalboxTriggerResult result) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startedAt);
        trigger_.end_trigger();
        if (result == RemotePalboxTriggerResult::opened) {
            consecutiveTimeoutCount_ = 0;
        } else if (elapsed > kTriggerTimeBudget) {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: remote palbox trigger took {} us (result={})\n"),
                elapsed.count(), static_cast<int>(result));
            ++consecutiveTimeoutCount_;
            if (consecutiveTimeoutCount_ >= kMaxConsecutiveTimeouts) {
                set_disabled("触发耗时连续超限，已停用远程终端");
            }
        }
        return result;
    };

    if (domainDisabled_) {
        return finish(RemotePalboxTriggerResult::disabled);
    }
    if (!probe_domain()) {
        return finish(RemotePalboxTriggerResult::unavailable);
    }

    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    auto* const controller = local_player_controller(worldContext);
    auto* const playerState = find_singleton(STR("PalPlayerState"));
    if (controller == nullptr || playerState == nullptr) {
        note("无法解析本地玩家状态");
        return finish(RemotePalboxTriggerResult::unavailable);
    }

    if (config_.disableInDungeon && call_bool(playerState, STR("IsInStage")).value_or(false)) {
        note("地牢内已禁用");
        return finish(RemotePalboxTriggerResult::blocked);
    }
    if (config_.disableWhileMounted && call_bool(controller, STR("IsRiding")).value_or(false)) {
        note("骑乘中已禁用");
        return finish(RemotePalboxTriggerResult::blocked);
    }
    if (config_.disableDuringCombat) {
        // 战斗检测函数在 dump 中未确认：依次探测候选名，全部不可用视为 false（fail-open）。
        const bool inCombat = call_bool(playerState, STR("IsInCombat")).value_or(false) ||
                              call_bool(playerState, STR("IsInBattle")).value_or(false) ||
                              call_bool(controller, STR("IsInCombat")).value_or(false);
        if (inCombat) {
            note("战斗中已禁用");
            return finish(RemotePalboxTriggerResult::blocked);
        }
    }

    auto* const manager = find_singleton(STR("PalBaseCampManager"));
    std::vector<FGuid> baseIds;
    if (manager == nullptr || !read_base_ids(manager, baseIds) || baseIds.empty()) {
        note("没有可用的已拥有基地");
        return finish(RemotePalboxTriggerResult::noBase);
    }

    auto* const mapObjectManager = find_singleton(STR("PalMapObjectManager"));
    std::vector<BaseCampCandidate> candidates;
    candidates.reserve(baseIds.size());
    for (const auto& baseId : baseIds) {
        UObject* model{};
        if (!try_get_base_model(manager, baseId, model)) {
            continue;
        }
        FGuid ownerMapObjectId{};
        if (!read_guid(model, STR("GetOwnerMapObjectInstanceId"), ownerMapObjectId)) {
            continue;
        }
        const bool playerInside =
            config_.onlyInsideBaseCircle
                ? player_inside_base(controller, model, mapObjectManager, ownerMapObjectId)
                : false;
        const double distanceSquared = config_.onlyInsideBaseCircle && playerInside ? 0.0 : 1.0e18;
        candidates.push_back({.id = std::to_string(baseId.A) + std::to_string(baseId.B),
                              .playerInside = playerInside,
                              .distanceSquared = distanceSquared});
    }
    const auto pick = select_remote_base_camp(candidates);
    if (!pick.has_value()) {
        note("没有可用的已拥有基地");
        return finish(RemotePalboxTriggerResult::noBase);
    }
    const auto& selectedBase = baseIds[*pick];
    UObject* selectedModel{};
    if (!try_get_base_model(manager, selectedBase, selectedModel)) {
        note("基地模型解析失败");
        return finish(RemotePalboxTriggerResult::unavailable);
    }
    FGuid ownerMapObjectId{};
    if (!read_guid(selectedModel, STR("GetOwnerMapObjectInstanceId"), ownerMapObjectId)) {
        note("终端实例解析失败");
        return finish(RemotePalboxTriggerResult::unavailable);
    }

    if (!resolve_widget_path(widgetPath_, widgetPathResolved_)) {
        note("PalBox 界面类未找到");
        return finish(RemotePalboxTriggerResult::unavailable);
    }
    const auto widePath = text_encoding::widen_ascii(widgetPath_);
    auto* const widgetClass =
        UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, widePath.c_str());
    if (widgetClass == nullptr) {
        note("PalBox 界面类解析失败");
        return finish(RemotePalboxTriggerResult::unavailable);
    }

    auto* const hudService = find_singleton(STR("PalHUDService"));
    auto* const createParamFunction =
        hudService == nullptr
            ? nullptr
            : hudService->GetFunctionByNameInChain(STR("CreateDispatchParameterForK2Node"));
    auto* const pushFunction =
        hudService == nullptr ? nullptr : hudService->GetFunctionByNameInChain(STR("Push"));
    if (hudService == nullptr || createParamFunction == nullptr || pushFunction == nullptr) {
        note("HUD 服务不可用");
        return finish(RemotePalboxTriggerResult::unavailable);
    }

    // CreateDispatchParameterForK2Node 的 ParameterClass 需要 UPalHUDDispatchParameter_PalBox 类。
    // 通过类名定位原生类对象（与 widget 类解析不同的路径：原生类在 /Script/Pal）。
    auto* const palBoxParamClass = UObjectGlobals::StaticFindObject<UClass*>(
        nullptr, nullptr, STR("/Script/Pal.UPalHUDDispatchParameter_PalBox"));
    if (palBoxParamClass == nullptr) {
        note("PalBox 参数类不可用");
        return finish(RemotePalboxTriggerResult::unavailable);
    }

    {
        pal_game::FunctionParams createParams{createParamFunction};
        auto* const contextProperty = CastField<FObjectPropertyBase>(
            createParamFunction->FindProperty(FName(STR("WorldContextObject"), FNAME_Find)));
        auto* const classInputProperty = CastField<FClassProperty>(
            createParamFunction->FindProperty(FName(STR("ParameterClass"), FNAME_Find)));
        auto* const createReturnProperty =
            CastField<FObjectPropertyBase>(createParamFunction->GetReturnProperty());
        if (contextProperty == nullptr || classInputProperty == nullptr ||
            createReturnProperty == nullptr) {
            note("HUD 参数工厂布局不可用");
            return finish(RemotePalboxTriggerResult::unavailable);
        }
        contextProperty->SetObjectPropertyValue(
            contextProperty->ContainerPtrToValuePtr<void>(createParams.data()), worldContext);
        classInputProperty->SetPropertyValueInContainer(createParams.data(), palBoxParamClass);
        hudService->ProcessEvent(createParamFunction, createParams.data());
        auto* const dispatchParameter = createReturnProperty->GetObjectPropertyValue(
            createReturnProperty->ContainerPtrToValuePtr<void>(createParams.data()));
        if (!pal_game::is_valid(dispatchParameter)) {
            note("HUD 参数对象创建失败");
            return finish(RemotePalboxTriggerResult::unavailable);
        }

        auto* const baseCampIdProperty = CastField<FStructProperty>(
            dispatchParameter->GetPropertyByNameInChain(STR("BaseCampId")));
        auto* const ownerProperty = CastField<FStructProperty>(
            dispatchParameter->GetPropertyByNameInChain(STR("OwnerMapObjectInstanceId")));
        if (baseCampIdProperty == nullptr || ownerProperty == nullptr) {
            note("PalBox 参数字段布局不可用");
            return finish(RemotePalboxTriggerResult::unavailable);
        }
        baseCampIdProperty->CopyCompleteValue(
            baseCampIdProperty->ContainerPtrToValuePtr<void>(dispatchParameter), &selectedBase);
        ownerProperty->CopyCompleteValue(
            ownerProperty->ContainerPtrToValuePtr<void>(dispatchParameter), &ownerMapObjectId);

        pal_game::FunctionParams pushParams{pushFunction};
        auto* const widgetClassProperty = CastField<FClassProperty>(
            pushFunction->FindProperty(FName(STR("WidgetClass"), FNAME_Find)));
        auto* const parameterProperty = CastField<FObjectPropertyBase>(
            pushFunction->FindProperty(FName(STR("Parameter"), FNAME_Find)));
        auto* const pushReturnProperty =
            CastField<FStructProperty>(pushFunction->GetReturnProperty());
        if (widgetClassProperty == nullptr || parameterProperty == nullptr ||
            pushReturnProperty == nullptr) {
            note("HUD Push 布局不可用");
            return finish(RemotePalboxTriggerResult::unavailable);
        }
        widgetClassProperty->SetPropertyValueInContainer(pushParams.data(), widgetClass);
        parameterProperty->SetObjectPropertyValue(
            parameterProperty->ContainerPtrToValuePtr<void>(pushParams.data()), dispatchParameter);
        hudService->ProcessEvent(pushFunction, pushParams.data());
        FGuid pushedWidgetId{};
        pushReturnProperty->CopyCompleteValue(
            &pushedWidgetId, pushReturnProperty->ContainerPtrToValuePtr<void>(pushParams.data()));
        if (is_zero_guid(pushedWidgetId)) {
            note("PalBox UI 打开失败（未返回有效 widget ID）");
            return finish(RemotePalboxTriggerResult::unavailable);
        }
    }

    {
        const std::lock_guard lock(snapshotMutex_);
        ++openCount_;
        lastMessage_ = "已打开 PalBox UI";
    }
    return finish(RemotePalboxTriggerResult::opened);
}

auto RemotePalboxRuntime::probe_domain() -> bool {
    if (domainProbed_) {
        return true;
    }
    domainProbed_ = true;
    auto* const hudService = find_singleton(STR("PalHUDService"));
    auto* const manager = find_singleton(STR("PalBaseCampManager"));
    const bool hudOk =
        hudService != nullptr &&
        hudService->GetFunctionByNameInChain(STR("CreateDispatchParameterForK2Node")) != nullptr &&
        hudService->GetFunctionByNameInChain(STR("Push")) != nullptr;
    const bool managerOk = manager != nullptr &&
                           manager->GetFunctionByNameInChain(STR("GetBaseCampIds")) != nullptr &&
                           manager->GetFunctionByNameInChain(STR("TryGetModel")) != nullptr;
    if (!hudOk || !managerOk) {
        set_disabled("关键反射点不可用，本世界已停用远程终端");
        return false;
    }
    return true;
}

auto RemotePalboxRuntime::set_disabled(const std::string& message) -> void {
    domainDisabled_ = true;
    note(message);
    Output::send<LogLevel::Warning>(STR("PalworldEditor: remote palbox disabled - {}\n"),
                                    text_encoding::widen_ascii(message));
}

auto RemotePalboxRuntime::note(const std::string& message) -> void {
    const std::lock_guard lock(snapshotMutex_);
    lastMessage_ = message;
    if (message.contains("已打开")) {
        return;
    }
    ++failCount_;
}
}  // namespace pal_remote_palbox
