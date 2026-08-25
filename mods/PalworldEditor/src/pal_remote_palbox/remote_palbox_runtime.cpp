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
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FSoftObjectPath.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <common/game_foreground.hpp>
#include <common/game_reflection.hpp>
#include <common/player_state_gate.hpp>
#include <common/text_encoding.hpp>
#include <game/pal_base_camp_reflection.hpp>
#include <pal_remote_palbox/remote_palbox_runtime.hpp>
#include <windows.h>

using namespace RC;
using namespace RC::Unreal;

namespace pal_remote_palbox {
namespace {

/** @brief 连续触发超时的次数上限；达到后停用域。 */
inline constexpr std::uint64_t kMaxConsecutiveTimeouts = 5;
/** @brief HUD 可堆叠窗口数组的防御性上限。 */
inline constexpr int32 kMaximumStackableWidgets = 4'096;

struct CreateDispatchParameterContract {
    FObjectPropertyBase* worldContextObject{};
    FClassProperty* parameterClass{};
    FObjectPropertyBase* returnValue{};
};

struct PushContract {
    FClassProperty* widgetClass{};
    FObjectPropertyBase* parameter{};
    FStructProperty* returnValue{};
};

[[nodiscard]] auto resolve_create_dispatch_parameter_contract(UFunction* function)
    -> std::optional<CreateDispatchParameterContract> {
    auto* const worldContextObject = function == nullptr
                                         ? nullptr
                                         : CastField<FObjectPropertyBase>(function->FindProperty(
                                               FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const parameterClass =
        function == nullptr ? nullptr
                            : CastField<FClassProperty>(
                                  function->FindProperty(FName(STR("ParameterClass"), FNAME_Find)));
    auto* const returnValue =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(
                                  function->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
    if (!pal_game::has_exact_parameter_count(function, 3) ||
        !pal_game::is_input_parameter(worldContextObject) ||
        !pal_game::is_input_parameter(parameterClass) ||
        !pal_game::is_return_parameter(returnValue)) {
        return std::nullopt;
    }
    return CreateDispatchParameterContract{.worldContextObject = worldContextObject,
                                           .parameterClass = parameterClass,
                                           .returnValue = returnValue};
}

[[nodiscard]] auto resolve_push_contract(UFunction* function) -> std::optional<PushContract> {
    auto* const widgetClass =
        function == nullptr ? nullptr
                            : CastField<FClassProperty>(
                                  function->FindProperty(FName(STR("WidgetClass"), FNAME_Find)));
    auto* const parameter = function == nullptr
                                ? nullptr
                                : CastField<FObjectPropertyBase>(
                                      function->FindProperty(FName(STR("Parameter"), FNAME_Find)));
    auto* const returnValue =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
    if (!pal_game::has_exact_parameter_count(function, 3) ||
        !pal_game::is_input_parameter(widgetClass) || !pal_game::is_input_parameter(parameter) ||
        !pal_game::is_return_parameter(returnValue) ||
        !pal_game::matches_struct_identity(returnValue, STR("Guid"), sizeof(FGuid))) {
        return std::nullopt;
    }
    return PushContract{
        .widgetClass = widgetClass, .parameter = parameter, .returnValue = returnValue};
}

/** @brief 选择策略输入与打开终端所需 GUID 的单一候选记录。 */
struct ResolvedBaseCampCandidate {
    BaseCampCandidate selection;
    FGuid baseId;
    FGuid ownerMapObjectId;
};

/** @brief 单次触发允许的软耗时上限；超过仅记录日志。 */
inline constexpr auto kTriggerTimeBudget = std::chrono::milliseconds(2);

/** @brief Push 返回零 GUID 后的有界界面确认窗口总时长。 */
inline constexpr auto kPushConfirmWindow = std::chrono::milliseconds(600);
/** @brief 确认窗口内的复查节流间隔。 */
inline constexpr auto kPushConfirmCheckInterval = std::chrono::milliseconds(150);

/** @brief 帕鲁存储菜单蓝图资产/生成类路径（Palworld 固定资产，与交互打开的箱子一致）。
 *  @details 该类属于延迟加载 UI 资产：未加载时对象数组中不存在，需按资产路径主动加载。 */
inline constexpr const wchar_t* kPalStorageWidgetAssetPath =
    L"/Game/Pal/Blueprint/UI/PalStorage/WBP_PalStorageMenu";
inline constexpr const wchar_t* kPalStorageWidgetClassPath =
    L"/Game/Pal/Blueprint/UI/PalStorage/WBP_PalStorageMenu.WBP_PalStorageMenu_C";

/** @brief 世界内对象类名 → 单例实例查找。 */
[[nodiscard]] auto find_singleton(const wchar_t* className) -> UObject* {
    return UObjectGlobals::FindFirstOf(className);
}

/** @brief 获取 Palworld 世界设置对象（PalUtility::GetGameSetting）。
 *  @details 与 pal_game::local_player_controller 同模式：PalUtility 是蓝图函数库，静态蓝图
 *           UFunction 在 CDO 上 ProcessEvent 调用；返回值持有 BaseCampAreaRange。 */
[[nodiscard]] auto get_game_setting(UObject* worldContext) -> UObject* {
    auto* utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* function =
        utility == nullptr ? nullptr : utility->GetFunctionByNameInChain(STR("GetGameSetting"));
    auto* input = function == nullptr ? nullptr
                                      : CastField<FObjectPropertyBase>(function->FindProperty(
                                            FName(STR("WorldContextObject"), FNAME_Find)));
    auto* output = function == nullptr ? nullptr
                                       : CastField<FObjectPropertyBase>(function->FindProperty(
                                             FName(STR("ReturnValue"), FNAME_Find)));
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 2) || !pal_game::is_input_parameter(input) ||
        !pal_game::is_return_parameter(output)) {
        return nullptr;
    }
    pal_game::FunctionParams params{function};
    input->SetObjectPropertyValue(input->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    utility->ProcessEvent(function, params.data());
    auto* const setting =
        output->GetObjectPropertyValue(output->ContainerPtrToValuePtr<void>(params.data()));
    return pal_game::is_valid(setting) ? setting : nullptr;
}

/** @brief 从模型 getter 读取 FGuid 字段。 */
[[nodiscard]] auto read_guid(UObject* model, const wchar_t* getterName, FGuid& output) -> bool {
    auto* function =
        pal_game::is_valid(model) ? model->GetFunctionByNameInChain(getterName) : nullptr;
    auto* returnProperty =
        function == nullptr ? nullptr : CastField<FStructProperty>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_return_parameter(returnProperty) ||
        !pal_game::matches_struct_identity(returnProperty, STR("Guid"), sizeof(FGuid))) {
        return false;
    }
    pal_game::FunctionParams params{function};
    model->ProcessEvent(function, params.data());
    returnProperty->CopyCompleteValue(&output,
                                      returnProperty->ContainerPtrToValuePtr<void>(params.data()));
    return output.A != 0 || output.B != 0 || output.C != 0 || output.D != 0;
}

/** @brief 从世界设置读取所有基地共用的视觉建设圈半径。 */
[[nodiscard]] auto read_world_build_area_range(UObject* worldContext, float& output) -> bool {
    auto* const setting = get_game_setting(worldContext);
    if (pal_game::is_valid(setting)) {
        auto* const property = setting->GetPropertyByNameInChain(STR("BaseCampAreaRange"));
        auto* const floatProperty = CastField<FFloatProperty>(property);
        if (floatProperty != nullptr) {
            output = floatProperty->GetPropertyValueInContainer(setting);
            return output > 0.0F;
        }
    }
    return false;
}

/** @brief 玩家当前位置（Pawn → K2_GetActorLocation）。 */
[[nodiscard]] auto read_player_location(UObject* controller, FVector& output) -> bool {
    auto* const pawn = pal_game::player_pawn(controller);
    if (pawn == nullptr) {
        return false;
    }
    return pal_game::read_actor_location(pawn, output);
}

/** @brief 读取基地模型中心（UPalBaseCampModel 的 Transform.Translation）。
 *  @details 镜像 AnywherePalbox：基地模型自带 Transform，物理 Palbox actor 未流加载时
 *           也可用，无需走 FindConcreteModel（concrete model 是 UObject 非 Actor，
 *           没有 GetLocation/K2_GetActorLocation）。优先 GetTransform() UFunction
 *           返回值，其次读 Transform 属性；Translation 通过 FStruct 反射按名取子字段，
 *           不依赖 FTransform 的 C++ 布局。 */
[[nodiscard]] auto read_model_center(UObject* model, FVector& output) -> bool {
    const auto readTranslation = [&output](FStructProperty* transformProperty,
                                           const void* container) -> bool {
        // 外层必须是 FTransform（GetTransform 返回值或 Transform 属性），内层
        // Translation 必须是 FVector；只验证 FStructProperty 会让任意同名字段被复制。
        if (!pal_game::matches_struct_identity(transformProperty, STR("Transform"),
                                               sizeof(FTransform))) {
            return false;
        }
        auto* const translation = transformProperty->GetStruct()->GetPropertyByNameInChain(
            FName(STR("Translation"), FNAME_Find));
        auto* const translationStruct = CastField<FStructProperty>(translation);
        if (!pal_game::matches_struct_identity(translationStruct, STR("Vector"), sizeof(FVector))) {
            return false;
        }
        translationStruct->CopyCompleteValue(
            &output, translationStruct->ContainerPtrToValuePtr<void>(container));
        return true;
    };
    if (pal_game::is_valid(model)) {
        auto* const function = model->GetFunctionByNameInChain(STR("GetTransform"));
        if (function != nullptr) {
            auto* const returnProperty = CastField<FStructProperty>(function->GetReturnProperty());
            if (pal_game::has_exact_parameter_count(function, 1) &&
                pal_game::is_return_parameter(returnProperty)) {
                pal_game::FunctionParams params{function};
                model->ProcessEvent(function, params.data());
                return readTranslation(returnProperty,
                                       returnProperty->ContainerPtrToValuePtr<void>(params.data()));
            }
        }
        auto* const transformProperty =
            CastField<FStructProperty>(model->GetPropertyByNameInChain(STR("Transform")));
        if (transformProperty != nullptr) {
            return readTranslation(transformProperty,
                                   transformProperty->ContainerPtrToValuePtr<void>(model));
        }
    }
    return false;
}

/** @brief 读取终端具体模型自带的界面类（UPalMapObjectBaseCampPoint::PalBoxWiget）。
 *  @details 与游戏交互打开终端时使用的界面类一致；TSubclassOf 反射为 FClassProperty。 */
[[nodiscard]] auto read_terminal_widget_class(UObject* concreteModel, UClass*& widgetClass)
    -> bool {
    widgetClass = nullptr;
    auto* const property = pal_game::is_valid(concreteModel)
                               ? concreteModel->GetPropertyByNameInChain(STR("PalBoxWiget"))
                               : nullptr;
    auto* const classProperty = CastField<FClassProperty>(property);
    if (classProperty == nullptr) {
        return false;
    }
    auto* const value = classProperty->GetObjectPropertyValue(
        classProperty->ContainerPtrToValuePtr<void>(concreteModel));
    widgetClass = static_cast<UClass*>(value);
    return widgetClass != nullptr;
}

/** @brief 获取帕鲁存储菜单生成类；未加载时按资产路径主动加载（UI 资产延迟加载）。
 *  @details 先查已加载的类对象；未命中则通过 AssetRegistry 加载资产包（TryLoad），
 *           加载完成后类对象必定在内存中，再按类路径查找。只在游戏线程调用。 */
[[nodiscard]] auto load_widget_class(UClass*& widgetClass) -> bool {
    widgetClass =
        UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kPalStorageWidgetClassPath);
    if (widgetClass != nullptr) {
        return true;
    }
    try {
        FSoftObjectPath softPath{FString{kPalStorageWidgetAssetPath}};
        static_cast<void>(softPath.TryLoad());  // 加载副作用：类对象进入内存
    } catch (const std::exception&) {
        return false;  // 资产注册表不可用：无法主动加载
    }
    widgetClass =
        UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kPalStorageWidgetClassPath);
    return widgetClass != nullptr;
}

/** @brief widget 是否在视口内（IsInViewport 为真）；函数不可用时返回 nullopt。
 *  @details 与 AnywherePalbox 一致：函数不可用的元素不视为打开（由调用方跳过）。
 *  @note 不能以 GetParmsSize()!=0 判定“有入参”：UFunction::ParmsSize 包含返回值槽位，
 *        任何带返回值的无参函数都 >0；这里只按函数名调用已知的无参函数。 */
[[nodiscard]] auto widget_is_in_viewport(UObject* widget) -> std::optional<bool> {
    return pal_game::invoke<bool>(widget, STR("IsInViewport"));
}

/** @brief 读取 ESlateVisibility 数值（GetVisibility 返回值）；不可用时返回 nullopt。
 *  @details UE5 的 UENUM 属性为 FEnumProperty（底层 uint8），兼容 FByteProperty。 */
[[nodiscard]] auto read_slate_visibility(UObject* widget) -> std::optional<std::uint8_t> {
    if (!pal_game::is_valid(widget)) {
        return std::nullopt;
    }
    auto* const function = widget->GetFunctionByNameInChain(STR("GetVisibility"));
    auto* const returnProperty = function == nullptr ? nullptr : function->GetReturnProperty();
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_return_parameter(returnProperty)) {
        return std::nullopt;
    }
    pal_game::FunctionParams params{function};
    if (auto* const enumProperty = CastField<FEnumProperty>(returnProperty);
        enumProperty != nullptr) {
        auto* const underlying = CastField<FByteProperty>(enumProperty->GetUnderlyingProperty());
        if (underlying == nullptr) {
            return std::nullopt;
        }
        widget->ProcessEvent(function, params.data());
        return underlying->GetPropertyValueInContainer(params.data());
    }
    if (auto* const byteProperty = CastField<FByteProperty>(returnProperty);
        byteProperty != nullptr) {
        widget->ProcessEvent(function, params.data());
        return byteProperty->GetPropertyValueInContainer(params.data());
    }
    return std::nullopt;
}

/** @brief 本地控制器对应的 HUD 上是否已有打开的菜单（防叠菜单）。
 *  @details 镜像 AnywherePalbox：检查 APalHUDInGame::StackableUIWidgets（Palworld 1.0
 *           dump 中唯一存在的栈式 UI 数组，强指针）逐元素检查 IsInViewport 且
 *           GetVisibility 非 Collapsed/Hidden；IsInViewport 不可用的元素跳过，
 *           全部元素都无法检查时 fail-closed 拦截；空数组不拦截。 */
[[nodiscard]] auto palbox_menu_is_open(UObject* controller) -> bool {
    auto* hud = [&]() -> UObject* {
        if (auto* const value =
                pal_game::invoke<UObject*>(controller, STR("GetHUD")).value_or(nullptr)) {
            return value;
        }
        auto* const property = pal_game::is_valid(controller)
                                   ? controller->GetPropertyByNameInChain(STR("MyHUD"))
                                   : nullptr;
        auto* const objectProperty = CastField<FObjectPropertyBase>(property);
        if (objectProperty == nullptr) {
            return nullptr;
        }
        auto* const value = objectProperty->GetObjectPropertyValue(
            objectProperty->ContainerPtrToValuePtr<void>(controller));
        return pal_game::is_valid(value) ? value : nullptr;
    }();
    if (hud == nullptr) {
        return false;
    }
    auto* const property = hud->GetPropertyByNameInChain(STR("StackableUIWidgets"));
    auto* const arrayProperty = CastField<FArrayProperty>(property);
    if (arrayProperty == nullptr) {
        return false;
    }
    auto* const innerProperty = CastField<FObjectPropertyBase>(arrayProperty->GetInner());
    if (innerProperty == nullptr) {
        return false;  // 非对象数组：无法逐元素判断
    }
    FScriptArrayHelper_InContainer helper{arrayProperty, hud};
    const auto count = helper.Num();
    if (count <= 0 || count > kMaximumStackableWidgets) {
        return false;
    }
    // 数组非空：逐个检查元素；IsInViewport 不可用的元素跳过（不视为打开）。
    bool inspectedAny = false;
    for (int32 index{}; index < count; ++index) {
        auto* const widget = innerProperty->GetObjectPropertyValue(helper.GetRawPtr(index));
        if (!pal_game::is_valid(widget)) {
            continue;  // 无效元素：跳过
        }
        const auto inViewport = widget_is_in_viewport(widget);
        if (!inViewport.has_value()) {
            continue;  // 无法检查视口状态：跳过该元素
        }
        inspectedAny = true;
        const auto visibility = read_slate_visibility(widget);
        // ESlateVisibility：Collapsed=1, Hidden=2 视为隐藏；读取失败视为可见。
        const bool hidden = visibility.has_value() && (*visibility == 1 || *visibility == 2);
        if (*inViewport && !hidden) {
            return true;  // 在视口且可见：确有菜单打开
        }
    }
    // 数组非空但所有元素都无法检查视口状态：fail-closed 防叠。
    if (!inspectedAny) {
        return true;
    }
    // 与 AnywherePalbox 一致的兜底：打开中的菜单都会显示鼠标光标。
    auto* const cursorProperty = pal_game::is_valid(controller)
                                     ? controller->GetPropertyByNameInChain(STR("bShowMouseCursor"))
                                     : nullptr;
    auto* const cursorBool = CastField<FBoolProperty>(cursorProperty);
    return cursorBool != nullptr && cursorBool->GetPropertyValueInContainer(controller);
}

}  // namespace

auto RemotePalboxRuntime::load_config(const std::string_view iniPath) -> void {
    std::string content;
    if (std::ifstream stream{std::string{iniPath}, std::ios::binary}; stream) {
        content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    const auto config = parse_remote_palbox_config(content);
    {
        const std::lock_guard lock(snapshotMutex_);
        iniPath_ = std::string{iniPath};
        config_ = config;
        lastMessage_ = "配置已加载" + std::string{content.empty() ? "（使用默认值）" : ""};
    }
    trigger_.reset();
}

auto RemotePalboxRuntime::set_config(const RemotePalboxConfig config) -> void {
    std::string iniPath;
    {
        const std::lock_guard lock(snapshotMutex_);
        config_ = config;
        iniPath = iniPath_;
    }
    // 按键状态机只允许游戏线程访问：标记后由下一帧 tick 重置。
    configDirty_.store(true, std::memory_order_release);
    if (!iniPath.empty()) {
        std::ofstream stream{iniPath, std::ios::binary | std::ios::trunc};
        if (stream) {
            stream << serialize_remote_palbox_config(config);
        } else {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: failed to write remote palbox config '{}'\n"),
                text_encoding::widen_ascii(iniPath));
        }
    }
}

auto RemotePalboxRuntime::tick(const float deltaSeconds,
                               const skill_editor::WorldSessionState& session) -> void {
    static_cast<void>(deltaSeconds);
    const bool guiRequest = requestedOpen_.exchange(false);
    if (configDirty_.exchange(false, std::memory_order_acquire)) {
        trigger_.reset();  // 键位变化：在游戏线程重置按键状态机。
    }
    RemotePalboxConfig config;
    {
        const std::lock_guard lock(snapshotMutex_);
        config = config_;
    }
    const bool pressed = (GetAsyncKeyState(config.hotkeyVk) & 0x8000) != 0;
    const bool foreground = pal_game::foreground_is_game();
    // 状态机每帧无条件推进：按下为真、松开/失焦为假。若只在按下时调用 update，
    // 松开的下降沿会丢失，pressed_ 将永远为真，此后所有按键都不会再触发。
    const bool edge = trigger_.update(std::chrono::steady_clock::now(), foreground && pressed);
    const bool triggered = guiRequest || edge;
    if (session.can_access_unreal()) {
        run_pending_push_confirm();  // Push 零 GUI 确认窗口（空计划为常量时间）。
    }
    if (!triggered) {
        return;
    }
    if (!session.can_access_unreal()) {
        note("世界尚未就绪", true);
        return;
    }
    static_cast<void>(execute_trigger(config));
}

auto RemotePalboxRuntime::request_open() -> void {
    requestedOpen_.store(true);
}

auto RemotePalboxRuntime::begin_world_transition() -> void {
    trigger_.reset();
    domainDisabled_.store(false, std::memory_order_release);
    domainProbed_ = false;
    pendingConfirm_ = {};
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
        .domainDisabled = domainDisabled_.load(std::memory_order_acquire),
        .lastMessage = lastMessage_,
        .openCount = openCount_,
        .failCount = failCount_,
    };
}

auto RemotePalboxRuntime::execute_trigger(const RemotePalboxConfig& config)
    -> RemotePalboxTriggerResult {
    const auto startedAt = std::chrono::steady_clock::now();
    const auto finish = [this, startedAt](const RemotePalboxTriggerResult result) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startedAt);
        trigger_.end_trigger();
        if (result == RemotePalboxTriggerResult::opened) {
            consecutiveTimeoutCount_ = 0;
        } else if (result == RemotePalboxTriggerResult::unavailable && domainProbed_ &&
                   elapsed > kTriggerTimeBudget) {
            // 只有结构故障（unavailable）连续超时才停用；blocked/noBase 是用户操作
            // 被门控拒绝或环境问题，即使耗时较长也不停用。
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

    if (domainDisabled_.load(std::memory_order_acquire)) {
        return finish(RemotePalboxTriggerResult::disabled);
    }
    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    auto* const controller = pal_game::local_player_controller(worldContext);
    auto* const playerState = find_singleton(STR("PalPlayerState"));
    if (controller == nullptr || playerState == nullptr) {
        note("无法解析本地玩家状态", true);
        return finish(RemotePalboxTriggerResult::unavailable);
    }

    // 镜像 AnywherePalbox 的 _isGameLoaded：服务器同步标志存在且为 false 时拦截；
    // 属性缺失（低版本/异常布局）视为已就绪，避免误拦截。
    auto* const syncProperty =
        playerState->GetPropertyByNameInChain(STR("bIsCompleteSyncPlayerFromServer_InClient"));
    auto* const syncBool = CastField<FBoolProperty>(syncProperty);
    if (syncBool != nullptr && !syncBool->GetPropertyValueInContainer(playerState)) {
        note("世界尚未就绪", true);
        return finish(RemotePalboxTriggerResult::blocked);
    }

    if (config.disableInDungeon) {
        const auto inStage = pal_game::invoke<bool>(playerState, STR("IsInStage"));
        if (!pal_game::state_gate_allows(inStage)) {
            note(inStage.has_value() ? "地牢内已禁用" : "地牢状态不可读，已拦截", true);
            return finish(RemotePalboxTriggerResult::blocked);
        }
    }
    if (config.disableWhileMounted) {
        const auto riding = pal_game::invoke<bool>(controller, STR("IsRiding"));
        if (!pal_game::state_gate_allows(riding)) {
            note(riding.has_value() ? "骑乘中已禁用" : "骑乘状态不可读，已拦截", true);
            return finish(RemotePalboxTriggerResult::blocked);
        }
    }
    if (config.disableDuringCombat) {
        const auto battle = pal_game::player_in_battle_mode(controller);
        if (!pal_game::state_gate_allows(battle)) {
            note(battle.has_value() ? "战斗中已禁用" : "战斗状态不可读，已拦截", true);
            return finish(RemotePalboxTriggerResult::blocked);
        }
    }
    // 已有界面打开时拒绝：避免在已有菜单上叠出第二个帕鲁箱。
    if (palbox_menu_is_open(controller)) {
        note("已有界面打开，请先关闭再使用远程终端", true);
        return finish(RemotePalboxTriggerResult::blocked);
    }

    if (!probe_domain()) {
        const bool disabled = domainDisabled_.load(std::memory_order_acquire);
        if (!disabled) {
            note("远程终端服务尚未就绪，请稍后重试", true);
        }
        return finish(disabled ? RemotePalboxTriggerResult::disabled
                               : RemotePalboxTriggerResult::unavailable);
    }

    auto* const manager = find_singleton(STR("PalBaseCampManager"));
    std::vector<FGuid> baseIds;
    if (manager == nullptr || !pal_base_camp_reflection::read_base_ids(manager, baseIds) ||
        baseIds.empty()) {
        note("没有可用的已拥有基地", true);
        return finish(RemotePalboxTriggerResult::noBase);
    }

    // 候选只保留本地公会的基地：多人环境下 GetBaseCampIds 返回世界全部基地，
    // 不过滤可能把终端开到其他公会头上；本地归属不可读按拦截处理（fail-closed）。
    FGuid localGuildId{};
    if (!pal_base_camp_reflection::resolve_local_guild_id(worldContext, localGuildId)) {
        note("无法确认基地归属，已拦截", true);
        return finish(RemotePalboxTriggerResult::blocked);
    }

    auto* const mapObjectManager = find_singleton(STR("PalMapObjectManager"));
    FVector playerLocation{};
    const bool havePlayerLocation = read_player_location(controller, playerLocation);
    // 视觉圈半径只认世界设置 BaseCampAreaRange；不可读时按拦截处理（fail-closed），
    // 不回退随据点等级膨胀的模型 AreaRange（那会放宽门控）。
    float worldAreaRange{};
    if (config.onlyInsideBaseCircle && !read_world_build_area_range(worldContext, worldAreaRange)) {
        note("世界建设圈半径不可读，已拦截", true);
        return finish(RemotePalboxTriggerResult::blocked);
    }
    std::vector<ResolvedBaseCampCandidate> candidates;
    candidates.reserve(baseIds.size());
    for (const auto& baseId : baseIds) {
        UObject* model{};
        if (!pal_base_camp_reflection::try_get_base_model(manager, baseId, model)) {
            continue;
        }
        FGuid ownerGuildId{};
        if (!read_guid(model, STR("GetGroupIdBelongTo"), ownerGuildId) ||
            ownerGuildId.A != localGuildId.A || ownerGuildId.B != localGuildId.B ||
            ownerGuildId.C != localGuildId.C || ownerGuildId.D != localGuildId.D) {
            continue;  // 非本地公会（或归属不可读）的基地不进入候选。
        }
        FGuid ownerMapObjectId{};
        if (!read_guid(model, STR("GetOwnerMapObjectInstanceId"), ownerMapObjectId)) {
            continue;
        }
        // 营地中心来自基地模型的 Transform（物理 Palbox actor 未流加载也可用）；
        // 解析失败时距离保持极大值（选择退化为第一个候选，与仅圈内可用时的 fail-closed 一致）。
        FVector campCenter{};
        const bool haveCenter = read_model_center(model, campCenter);
        double distanceSquared = 1.0e18;
        if (haveCenter && havePlayerLocation) {
            const double dx = playerLocation.X() - campCenter.X();
            const double dy = playerLocation.Y() - campCenter.Y();
            distanceSquared = (dx * dx) + (dy * dy);
        }
        // 圈内判定 = 玩家到圈心距离 ≤ 建设圈半径（世界设置 BaseCampAreaRange，即视觉圈）。
        // 不能使用据点模型 AreaRange（随据点等级膨胀）或 InsideBaseCampCheckComponent
        // （检测圈含额外工作范围），两者都大于视觉圈。
        bool playerInside = false;
        if (config.onlyInsideBaseCircle) {
            playerInside = haveCenter && havePlayerLocation &&
                           distanceSquared <= static_cast<double>(worldAreaRange) *
                                                  static_cast<double>(worldAreaRange);
        }
        candidates.push_back({
            .selection = {.playerInside = playerInside, .distanceSquared = distanceSquared},
            .baseId = baseId,
            .ownerMapObjectId = ownerMapObjectId,
        });
    }
    std::vector<BaseCampCandidate> selectionCandidates;
    selectionCandidates.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        selectionCandidates.push_back(candidate.selection);
    }
    const auto pick = select_remote_base_camp(selectionCandidates);
    if (!pick.has_value()) {
        note("没有可用的已拥有基地", true);
        return finish(RemotePalboxTriggerResult::noBase);
    }
    if (config.onlyInsideBaseCircle && !candidates[*pick].selection.playerInside) {
        // 与配置项"仅基地圈内可用"语义一致：圈外不打开任何基地的终端。
        note("仅基地圈内可用，请站到基地圈内再使用", true);
        return finish(RemotePalboxTriggerResult::blocked);
    }
    const auto& selectedBase = candidates[*pick].baseId;
    const auto& ownerMapObjectId = candidates[*pick].ownerMapObjectId;

    // 界面类解析优先级：缓存路径（跨世界保留）→ 终端模型自带的 PalBoxWiget →
    // 按资产路径主动加载。任一路径得到 UClass* 后仅在本次触发内使用，跨帧只保留路径字符串。
    UClass* widgetClass{};
    if (!widgetPath_.empty()) {
        const auto widePath = text_encoding::widen_ascii(widgetPath_);
        widgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, widePath.c_str());
    }
    if (widgetClass == nullptr && mapObjectManager != nullptr) {
        UObject* terminalConcrete{};
        if (pal_base_camp_reflection::find_concrete_model(mapObjectManager, ownerMapObjectId,
                                                          terminalConcrete) &&
            read_terminal_widget_class(terminalConcrete, widgetClass)) {
            widgetPath_ = text_encoding::to_utf8(std::wstring{widgetClass->GetPathName()});
        }
    }
    if (widgetClass == nullptr && load_widget_class(widgetClass)) {
        widgetPath_ = text_encoding::to_utf8(std::wstring{widgetClass->GetPathName()});
    }
    if (widgetClass == nullptr) {
        note("终端界面类加载失败", true);
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
        note("HUD 服务不可用", true);
        return finish(RemotePalboxTriggerResult::unavailable);
    }

    // CreateDispatchParameterForK2Node 的 ParameterClass 需要 PalHUDDispatchParameter_PalBox 类。
    // 通过类名定位原生类对象（与 widget 类解析不同的路径：原生类在 /Script/Pal，不带 U 前缀）。
    auto* const palBoxParamClass = UObjectGlobals::StaticFindObject<UClass*>(
        nullptr, nullptr, STR("/Script/Pal.PalHUDDispatchParameter_PalBox"));
    if (palBoxParamClass == nullptr) {
        note("PalBox 参数类不可用", true);
        return finish(RemotePalboxTriggerResult::unavailable);
    }

    {
        const auto createContract = resolve_create_dispatch_parameter_contract(createParamFunction);
        if (!createContract.has_value()) {
            note("HUD 参数工厂布局不可用", true);
            return finish(RemotePalboxTriggerResult::unavailable);
        }
        // TSubclassOf<T> 约束由 FClassProperty::MetaClass 承载：传入的 PalBox 参数类
        // 必须是该元类的子类，否则引擎侧可能按不兼容布局消费参数对象。
        auto* const parameterMetaClass = createContract->parameterClass->GetMetaClass().Get();
        if (parameterMetaClass == nullptr || !palBoxParamClass->IsA(parameterMetaClass)) {
            note("HUD 参数类元数据不兼容", true);
            return finish(RemotePalboxTriggerResult::unavailable);
        }
        pal_game::FunctionParams createParams{createParamFunction};
        createContract->worldContextObject->SetObjectPropertyValue(
            createContract->worldContextObject->ContainerPtrToValuePtr<void>(createParams.data()),
            worldContext);
        createContract->parameterClass->SetPropertyValueInContainer(createParams.data(),
                                                                    palBoxParamClass);
        hudService->ProcessEvent(createParamFunction, createParams.data());
        auto* const dispatchParameter = createContract->returnValue->GetObjectPropertyValue(
            createContract->returnValue->ContainerPtrToValuePtr<void>(createParams.data()));
        if (!pal_game::is_valid(dispatchParameter) || !dispatchParameter->IsA(palBoxParamClass)) {
            note("HUD 参数对象创建失败或类型不符", true);
            return finish(RemotePalboxTriggerResult::unavailable);
        }

        auto* const baseCampIdProperty = CastField<FStructProperty>(
            dispatchParameter->GetPropertyByNameInChain(STR("BaseCampId")));
        auto* const ownerProperty = CastField<FStructProperty>(
            dispatchParameter->GetPropertyByNameInChain(STR("OwnerMapObjectInstanceId")));
        if (!pal_game::matches_struct_identity(baseCampIdProperty, STR("Guid"), sizeof(FGuid)) ||
            !pal_game::matches_struct_identity(ownerProperty, STR("Guid"), sizeof(FGuid))) {
            note("PalBox 参数字段布局不可用", true);
            return finish(RemotePalboxTriggerResult::unavailable);
        }
        baseCampIdProperty->CopyCompleteValue(
            baseCampIdProperty->ContainerPtrToValuePtr<void>(dispatchParameter), &selectedBase);
        ownerProperty->CopyCompleteValue(
            ownerProperty->ContainerPtrToValuePtr<void>(dispatchParameter), &ownerMapObjectId);

        const auto pushContract = resolve_push_contract(pushFunction);
        if (!pushContract.has_value()) {
            note("HUD Push 布局不可用", true);
            return finish(RemotePalboxTriggerResult::unavailable);
        }
        auto* const pushParameterClass = pushContract->parameter->GetPropertyClass().Get();
        if (pushParameterClass == nullptr || !dispatchParameter->IsA(pushParameterClass)) {
            note("HUD Push 参数类不兼容", true);
            return finish(RemotePalboxTriggerResult::unavailable);
        }
        auto* const widgetMetaClass = pushContract->widgetClass->GetMetaClass().Get();
        if (widgetMetaClass == nullptr || !widgetClass->IsA(widgetMetaClass)) {
            note("HUD Push 界面类元数据不兼容", true);
            return finish(RemotePalboxTriggerResult::unavailable);
        }
        pal_game::FunctionParams pushParams{pushFunction};
        pushContract->widgetClass->SetPropertyValueInContainer(pushParams.data(), widgetClass);
        pushContract->parameter->SetObjectPropertyValue(
            pushContract->parameter->ContainerPtrToValuePtr<void>(pushParams.data()),
            dispatchParameter);
        hudService->ProcessEvent(pushFunction, pushParams.data());
        FGuid widgetId{};
        pushContract->returnValue->CopyCompleteValue(
            &widgetId, pushContract->returnValue->ContainerPtrToValuePtr<void>(pushParams.data()));
        if (widgetId.is_valid()) {
            note("已打开 PalBox UI", false);
        } else {
            // Push 可异步完成（界面已入栈时 widget ID 仍可能全零，develop 时期已记录）：
            // 零 GUID 不判失败也不计结构故障，改为有界确认窗口内复查界面是否实际入栈；
            // 超时才记失败，且不停用域。
            const auto now = std::chrono::steady_clock::now();
            pendingConfirm_ = {.active = true,
                               .deadline = now + kPushConfirmWindow,
                               .nextCheck = now + kPushConfirmCheckInterval};
            note("已请求打开 PalBox UI，等待界面确认", false);
        }
    }

    {
        const std::lock_guard lock(snapshotMutex_);
        ++openCount_;
    }
    return finish(RemotePalboxTriggerResult::opened);
}

auto RemotePalboxRuntime::run_pending_push_confirm() -> void {
    if (!pendingConfirm_.active) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < pendingConfirm_.nextCheck) {
        return;
    }
    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    auto* const controller = pal_game::local_player_controller(worldContext);
    if (controller == nullptr) {
        // 世界上下文暂不可用：窗口内顺延，不做结论；超时只能按未确认记失败。
        if (now >= pendingConfirm_.deadline) {
            pendingConfirm_.active = false;
            note("HUD Push 确认窗口内无法解析玩家控制器", true);
        }
        return;
    }
    if (palbox_menu_is_open(controller)) {
        pendingConfirm_.active = false;
        note("已打开 PalBox UI", false);
        return;
    }
    if (now >= pendingConfirm_.deadline) {
        pendingConfirm_.active = false;
        note("HUD Push 未能在确认窗口内打开界面", true);
        return;
    }
    pendingConfirm_.nextCheck = now + kPushConfirmCheckInterval;
}

auto RemotePalboxRuntime::probe_domain() -> bool {
    if (domainProbed_) {
        return true;
    }
    auto* const hudService = find_singleton(STR("PalHUDService"));
    auto* const manager = find_singleton(STR("PalBaseCampManager"));
    if (hudService == nullptr || manager == nullptr) {
        return false;
    }
    const bool hudOk =
        resolve_create_dispatch_parameter_contract(
            hudService->GetFunctionByNameInChain(STR("CreateDispatchParameterForK2Node")))
            .has_value() &&
        resolve_push_contract(hudService->GetFunctionByNameInChain(STR("Push"))).has_value();
    const bool managerOk = manager->GetFunctionByNameInChain(STR("GetBaseCampIds")) != nullptr &&
                           manager->GetFunctionByNameInChain(STR("TryGetModel")) != nullptr;
    domainProbed_ = true;
    if (!hudOk || !managerOk) {
        set_disabled("关键反射点不可用，本世界已停用远程终端");
        return false;
    }
    return true;
}

auto RemotePalboxRuntime::set_disabled(const std::string& message) -> void {
    domainDisabled_.store(true, std::memory_order_release);
    note(message, true);
    Output::send<LogLevel::Warning>(STR("PalworldEditor: remote palbox disabled - {}\n"),
                                    text_encoding::widen_ascii(message));
}

auto RemotePalboxRuntime::note(const std::string& message, const bool isFailure) -> void {
    const std::lock_guard lock(snapshotMutex_);
    lastMessage_ = message;
    if (isFailure) {
        ++failCount_;
    }
}
}  // namespace pal_remote_palbox
