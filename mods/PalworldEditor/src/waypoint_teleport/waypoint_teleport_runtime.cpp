/**
 * @file waypoint_teleport_runtime.cpp
 * @brief 实现传送至最近地图标记点：门控、CustomMarkers 读取与原生 SyncTeleport。
 * @details 复用 pal_game 公共原语（控制器/Pawn/战斗判定/按键状态机）；标记 TMap 与
 *          传送参数全部经 FProperty API 按名读写，不手写参数布局。跨帧只保存纯值。
 */
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <optional>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <common/game_reflection.hpp>
#include <common/text_encoding.hpp>
#include <waypoint_teleport/waypoint_teleport_runtime.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace waypoint_teleport {
using namespace RC;
using namespace RC::Unreal;

namespace {

/** @brief 自定义标记 Map 最大索引的防御性上限（稀疏槽位不超过条目数的两倍）。 */
inline constexpr int32 kMaximumCustomMarkerIndex{static_cast<int32>(kMaximumCustomMarkers * 2)};
/** @brief 超过此水平距离（cm）的目标按"远距两段式"处理：目标区块大概率未流送。 */
inline constexpr double kDirectPlaceDistanceCm{10000.0};
/** @brief 第二段贴地等待时长：给区块流送留出时间。 */
inline constexpr auto kRefinementDelay = std::chrono::milliseconds{1200};
/** @brief 校正 Z 与第一段锚点的最小差值（cm）：小于该值视为已正确、免二次放置。 */
inline constexpr double kRefinementMinDeltaCm{100.0};

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

/** @brief 解析 LocationManager（PalUtility:GetLocationManager）。 */
[[nodiscard]] auto location_manager(UObject* worldContext) -> UObject* {
    auto* const utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* const function =
        utility == nullptr ? nullptr : utility->GetFunctionByNameInChain(STR("GetLocationManager"));
    auto* const input = function == nullptr ? nullptr
                                            : CastField<FObjectPropertyBase>(function->FindProperty(
                                                  FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const output = function == nullptr
                             ? nullptr
                             : CastField<FObjectPropertyBase>(
                                   function->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 2) || !pal_game::is_input_parameter(input) ||
        !pal_game::is_return_parameter(output)) {
        return nullptr;
    }
    pal_game::FunctionParams params{function};
    input->SetObjectPropertyValue(input->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    utility->ProcessEvent(function, params.data());
    auto* const manager =
        output->GetObjectPropertyValue(output->ContainerPtrToValuePtr<void>(params.data()));
    return pal_game::is_valid(manager) ? manager : nullptr;
}

/**
 * @brief 读取全部自定义标记的候选坐标。
 * @details TMap<FGuid, FPalCustomMarkerSaveData> 经 FMapProperty 布局迭代；值结构的
 *          IconLocation/IconType 按名解析。结构不兼容返回 false（调用方安全停用）。
 */
[[nodiscard]] auto read_marker_candidates(UObject* manager, std::vector<MarkerCandidate>& output)
    -> bool {
    auto* const mapProperty = CastField<FMapProperty>(
        pal_game::is_valid(manager) ? manager->GetPropertyByNameInChain(STR("CustomMarkers"))
                                    : nullptr);
    auto* const keyProperty =
        mapProperty == nullptr ? nullptr : CastField<FStructProperty>(mapProperty->GetKeyProp());
    auto* const valueProperty =
        mapProperty == nullptr ? nullptr : CastField<FStructProperty>(mapProperty->GetValueProp());
    auto* const map =
        mapProperty == nullptr ? nullptr : mapProperty->ContainerPtrToValuePtr<FScriptMap>(manager);
    if (mapProperty == nullptr || keyProperty == nullptr || valueProperty == nullptr ||
        map == nullptr || keyProperty->GetElementSize() != sizeof(FGuid)) {
        return false;
    }
    auto* const valueStruct = valueProperty->GetStruct().Get();
    auto* const locationProperty = valueStruct == nullptr
                                       ? nullptr
                                       : CastField<FStructProperty>(valueStruct->FindProperty(
                                             FName(STR("IconLocation"), FNAME_Find)));
    auto* const typeProperty =
        valueStruct == nullptr ? nullptr
                               : CastField<FIntProperty>(
                                     valueStruct->FindProperty(FName(STR("IconType"), FNAME_Find)));
    if (locationProperty == nullptr || typeProperty == nullptr ||
        locationProperty->GetElementSize() != sizeof(FVector)) {
        return false;
    }

    const int32 entryCount = map->Num();
    const int32 maximumIndex = map->GetMaxIndex();
    if (entryCount < 0 || entryCount > static_cast<int32>(kMaximumCustomMarkers) ||
        maximumIndex < 0 || maximumIndex > kMaximumCustomMarkerIndex) {
        return false;
    }

    const auto layout =
        FScriptMap::GetScriptLayout(keyProperty->GetSize(), keyProperty->GetMinAlignment(),
                                    valueProperty->GetSize(), valueProperty->GetMinAlignment());
    output.clear();
    output.reserve(static_cast<std::size_t>(entryCount));
    for (int32 index{}; index < maximumIndex; ++index) {
        if (!map->IsValidIndex(index)) {
            continue;
        }
        auto* const entry = map->GetData(index, layout);
        auto* const value = static_cast<void*>(static_cast<std::byte*>(entry) + layout.ValueOffset);
        std::array<std::uint32_t, 4> guid{};
        keyProperty->CopyCompleteValue(guid.data(), entry);  // 键始终位于条目起始偏移
        FVector location{};
        locationProperty->CopyCompleteValue(&location, value);
        output.push_back(
            MarkerCandidate{.guid = guid, .x = location.X(), .y = location.Y(), .z = location.Z()});
    }
    return true;
}

/** @brief 调用目标上的无参 void UFunction；函数缺失或带参数时跳过。 */
auto call_no_parameter_function(UObject* target, const wchar_t* functionName) -> void {
    auto* const function =
        pal_game::is_valid(target) ? target->GetFunctionByNameInChain(functionName) : nullptr;
    if (!pal_game::has_exact_parameter_count(function, 0)) {
        return;
    }
    target->ProcessEvent(function, nullptr);
}

/**
 * @brief 调用地图控件的 RemoveCustomIcon(Icon)。
 * @details 蓝图函数，参数名未知；按"唯一非返回参数必须是对象输入"验证签名，
 *          不兼容时跳过（后续的 TMap 移除仍会执行）。
 */
auto call_remove_custom_icon(UObject* map, UObject* icon) -> void {
    auto* const function =
        pal_game::is_valid(map) ? map->GetFunctionByNameInChain(STR("RemoveCustomIcon")) : nullptr;
    FObjectPropertyBase* inputProperty{};
    std::size_t parmCount{};
    if (function != nullptr) {
        for (auto* property :
             TFieldRange<FProperty>(function, EFieldIterationFlags::IncludeDeprecated)) {
            if (!property->HasAnyPropertyFlags(CPF_Parm)) {
                continue;
            }
            ++parmCount;
            if (pal_game::is_return_parameter(property)) {
                continue;
            }
            if (inputProperty != nullptr) {
                inputProperty = nullptr;
                break;
            }
            inputProperty = CastField<FObjectPropertyBase>(property);
        }
    }
    if (parmCount == 0 || parmCount > 2 || inputProperty == nullptr ||
        !pal_game::is_input_parameter(inputProperty)) {
        return;
    }
    pal_game::FunctionParams params{function};
    inputProperty->SetObjectPropertyValue(
        inputProperty->ContainerPtrToValuePtr<void>(params.data()), icon);
    map->ProcessEvent(function, params.data());
}

/**
 * @brief 从地图控件（WBP_Map_Base_C）的 CustomMarkerMap 移除标记图标。
 * @details RemoveLocalCustomMarker 只清 LocationManager 数据层；地图控件的图标
 *          TMap 是独立容器，不移除则地图上标记继续显示（参考实现两层都删）。
 *          每次传送的按键请求路径才执行一次；best-effort，控件缺失或结构
 *          不符时跳过，不影响数据层删除结果。
 */
auto remove_map_icons(const std::array<std::uint32_t, 4>& guid) -> void {
    std::vector<UObject*> maps;
    UObjectGlobals::FindAllOf(STR("WBP_Map_Base_C"), maps);
    for (auto* const map : maps) {
        auto* const mapProperty = CastField<FMapProperty>(
            pal_game::is_valid(map) ? map->GetPropertyByNameInChain(STR("CustomMarkerMap"))
                                    : nullptr);
        auto* const keyProperty = mapProperty == nullptr
                                      ? nullptr
                                      : CastField<FStructProperty>(mapProperty->GetKeyProp());
        auto* const valueProperty =
            mapProperty == nullptr ? nullptr
                                   : CastField<FObjectPropertyBase>(mapProperty->GetValueProp());
        auto* const scriptMap =
            mapProperty == nullptr ? nullptr : mapProperty->ContainerPtrToValuePtr<FScriptMap>(map);
        if (keyProperty == nullptr || valueProperty == nullptr || scriptMap == nullptr ||
            keyProperty->GetElementSize() != sizeof(FGuid)) {
            continue;
        }
        const int32 maximumIndex = scriptMap->GetMaxIndex();
        if (maximumIndex < 0 || maximumIndex > kMaximumCustomMarkerIndex) {
            continue;
        }
        const auto layout =
            FScriptMap::GetScriptLayout(keyProperty->GetSize(), keyProperty->GetMinAlignment(),
                                        valueProperty->GetSize(), valueProperty->GetMinAlignment());
        for (int32 index{}; index < maximumIndex; ++index) {
            if (!scriptMap->IsValidIndex(index)) {
                continue;
            }
            auto* const entry = scriptMap->GetData(index, layout);
            std::array<std::uint32_t, 4> entryGuid{};
            keyProperty->CopyCompleteValue(entryGuid.data(), entry);
            if (entryGuid != guid) {
                continue;
            }
            auto* const icon = valueProperty->GetObjectPropertyValue(
                static_cast<void*>(static_cast<std::byte*>(entry) + layout.ValueOffset));
            if (pal_game::is_valid(icon)) {
                call_remove_custom_icon(map, icon);
                call_no_parameter_function(icon, STR("RemoveFromParent"));
            }
            scriptMap->RemoveAt(index, layout);
            break;  // 同一控件内一个 GUID 至多一条
        }
    }
}

/** @brief 删除一个自定义地图标记（数据层 RemoveLocalCustomMarker + 地图图标）；best-effort。 */
[[nodiscard]] auto remove_custom_marker(UObject* manager, const std::array<std::uint32_t, 4>& guid)
    -> bool {
    auto* const function = pal_game::is_valid(manager)
                               ? manager->GetFunctionByNameInChain(STR("RemoveLocalCustomMarker"))
                               : nullptr;
    auto* const parameter = function == nullptr ? nullptr
                                                : CastField<FStructProperty>(function->FindProperty(
                                                      FName(STR("MarkerID"), FNAME_Find)));
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_input_parameter(parameter) ||
        parameter->GetElementSize() != sizeof(std::uint32_t) * 4 ||
        function->GetReturnProperty() != nullptr) {
        return false;
    }
    pal_game::FunctionParams params{function};
    parameter->CopyCompleteValue(parameter->ContainerPtrToValuePtr<void>(params.data()),
                                 guid.data());
    manager->ProcessEvent(function, params.data());
    remove_map_icons(guid);
    return true;
}

/**
 * @brief 对标记水平位置做垂直射线追踪，取真实地面高度。
 * @details 地图标记的 Z 不可靠（可能落在地表下方导致传送入地）；KismetSystemLibrary:
 *          LineTraceSingle（PalSquadAllOut 参考实现同款，通道 0）以标记 Z 为中心
 *          ±1km 窗口向下追踪，命中返回 ImpactPoint.Z。返回三态：有值=地面 Z、
 *          false=未命中、nullopt=结构不兼容。
 */
[[nodiscard]] auto trace_ground_z(UObject* worldContext, const double x, const double y,
                                  const double markerZ) -> std::optional<std::optional<double>> {
    constexpr double kTraceWindow = 100000.0;  // ±1km 覆盖任意标记 Z 误差
    auto* const library = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
    auto* const function =
        library == nullptr ? nullptr : library->GetFunctionByNameInChain(STR("LineTraceSingle"));
    if (!pal_game::is_valid(library) || !pal_game::has_exact_parameter_count(function, 13)) {
        return std::nullopt;
    }
    auto* const contextProperty = CastField<FObjectPropertyBase>(
        function->FindProperty(FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const startProperty =
        CastField<FStructProperty>(function->FindProperty(FName(STR("Start"), FNAME_Find)));
    auto* const endProperty =
        CastField<FStructProperty>(function->FindProperty(FName(STR("End"), FNAME_Find)));
    auto* const channelProperty =
        CastField<FByteProperty>(function->FindProperty(FName(STR("TraceChannel"), FNAME_Find)));
    auto* const ignoreSelfProperty =
        CastField<FBoolProperty>(function->FindProperty(FName(STR("bIgnoreSelf"), FNAME_Find)));
    auto* const outHitProperty =
        CastField<FStructProperty>(function->FindProperty(FName(STR("OutHit"), FNAME_Find)));
    auto* const resultProperty = CastField<FBoolProperty>(function->GetReturnProperty());
    if (!pal_game::is_input_parameter(contextProperty) ||
        !pal_game::is_input_parameter(startProperty) ||
        !pal_game::is_input_parameter(endProperty) || channelProperty == nullptr ||
        !pal_game::is_input_parameter(ignoreSelfProperty) ||
        !pal_game::is_output_parameter(outHitProperty) ||
        !pal_game::is_return_parameter(resultProperty) ||
        startProperty->GetElementSize() != sizeof(FVector) ||
        endProperty->GetElementSize() != sizeof(FVector)) {
        return std::nullopt;
    }
    auto* const hitStruct = outHitProperty->GetStruct().Get();
    auto* const impactProperty =
        hitStruct == nullptr ? nullptr
                             : CastField<FStructProperty>(
                                   hitStruct->FindProperty(FName(STR("ImpactPoint"), FNAME_Find)));
    if (impactProperty == nullptr || impactProperty->GetElementSize() != sizeof(FVector)) {
        return std::nullopt;
    }

    FVector start{};
    start.SetX(x);
    start.SetY(y);
    start.SetZ(markerZ + kTraceWindow);
    FVector end{};
    end.SetX(x);
    end.SetY(y);
    end.SetZ(markerZ - kTraceWindow);

    pal_game::FunctionParams params{function};
    contextProperty->SetObjectPropertyValue(
        contextProperty->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    startProperty->CopyCompleteValue(startProperty->ContainerPtrToValuePtr<void>(params.data()),
                                     &start);
    endProperty->CopyCompleteValue(endProperty->ContainerPtrToValuePtr<void>(params.data()), &end);
    channelProperty->SetPropertyValueInContainer(
        params.data(), static_cast<std::uint8_t>(0));  // 通道 0（PalSquad 实证）
    ignoreSelfProperty->SetPropertyValueInContainer(params.data(), true);
    library->ProcessEvent(function, params.data());
    if (!resultProperty->GetPropertyValueInContainer(params.data())) {
        return std::optional<double>{};  // 未命中：空 optional 内层
    }
    // ImpactPoint 是 FHitResult 内部字段：容器必须是 OutHit 在参数缓冲中的实例，
    // 而不是参数缓冲本身（否则读到 Start/End 区域的错位数据）。
    void* const hitInstance = outHitProperty->ContainerPtrToValuePtr<void>(params.data());
    FVector impact{};
    impactProperty->CopyCompleteValue(&impact,
                                      impactProperty->ContainerPtrToValuePtr<void>(hitInstance));
    return std::optional<double>{impact.Z()};
}

/**
 * @brief 把坠落伤害的"下落起点"重置到当前位置（游戏原生机制）。
 * @details Palworld 按 LastJumpedLocation 与落点的高度差结算坠落伤害；传送不会
 *          更新该起点，导致空投整段被计为坠落。SetNoFallDamageHeightLastJumpedLocation
 *          是游戏跳跃/滑翔使用的原生入口。best-effort：失败仅保持现状。
 */
[[nodiscard]] auto reset_fall_origin(UObject* pawn) -> bool {
    auto* const componentProperty =
        pal_game::is_valid(pawn) ? CastField<FObjectPropertyBase>(pawn->GetPropertyByNameInChain(
                                       STR("CharacterParameterComponent")))
                                 : nullptr;
    auto* const component = componentProperty == nullptr
                                ? nullptr
                                : componentProperty->GetObjectPropertyValue(
                                      componentProperty->ContainerPtrToValuePtr<void>(pawn));
    auto* const parameter =
        pal_game::invoke<UObject*>(component, STR("GetIndividualParameter")).value_or(nullptr);
    if (!pal_game::is_valid(parameter)) {
        return false;
    }
    auto* const function =
        parameter->GetFunctionByNameInChain(STR("SetNoFallDamageHeightLastJumpedLocation"));
    if (!pal_game::has_exact_parameter_count(function, 0) ||
        function->GetReturnProperty() != nullptr) {
        return false;
    }
    pal_game::FunctionParams params{function};
    parameter->ProcessEvent(function, params.data());
    return true;
}

/**
 * @brief 调用 AActor::K2_SetActorLocation(FVector, bool, FHitResult&, bool) -> bool。
 * @details bSweep=false 的无扫掠精确放置（PalSquadAllOut 用同族函数移动帕鲁）。
 *          此前两版原语的教训：SyncTeleport 为有状态序列入口（EngineTick 前置相位
 *          下三次实测内部 -1 崩溃）；K2_TeleportTo 带路径扫掠——玩家到目标的直线
 *          穿过山体时在阻挡点停下，把玩家放进地形内（实测"首次入地、二次正常"）。
 *          无扫掠放置与路径无关，落点由本 mod 的地面追踪 + 离地间隙保证安全。
 */
[[nodiscard]] auto call_set_actor_location(UObject* pawn, const FVector& destination)
    -> std::optional<bool> {
    auto* const function = pal_game::is_valid(pawn)
                               ? pawn->GetFunctionByNameInChain(STR("K2_SetActorLocation"))
                               : nullptr;
    auto* const locationProperty =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("NewLocation"), FNAME_Find)));
    auto* const sweepProperty =
        function == nullptr
            ? nullptr
            : CastField<FBoolProperty>(function->FindProperty(FName(STR("bSweep"), FNAME_Find)));
    auto* const hitProperty =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("SweepHitResult"), FNAME_Find)));
    auto* const teleportProperty =
        function == nullptr
            ? nullptr
            : CastField<FBoolProperty>(function->FindProperty(FName(STR("bTeleport"), FNAME_Find)));
    auto* const resultProperty =
        function == nullptr ? nullptr : CastField<FBoolProperty>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 5) ||
        !pal_game::is_input_parameter(locationProperty) ||
        !pal_game::is_input_parameter(sweepProperty) ||
        !pal_game::is_output_parameter(hitProperty) ||
        !pal_game::is_input_parameter(teleportProperty) ||
        !pal_game::is_return_parameter(resultProperty) ||
        locationProperty->GetElementSize() != sizeof(FVector)) {
        return std::nullopt;
    }

    pal_game::FunctionParams params{function};
    locationProperty->CopyCompleteValue(
        locationProperty->ContainerPtrToValuePtr<void>(params.data()), &destination);
    sweepProperty->SetPropertyValueInContainer(params.data(), false);
    teleportProperty->SetPropertyValueInContainer(params.data(), true);
    pawn->ProcessEvent(function, params.data());
    return resultProperty->GetPropertyValueInContainer(params.data());
}
}  // namespace

auto WaypointTeleportRuntime::load_config(const std::string_view iniPath) -> void {
    std::string content;
    if (std::ifstream stream{std::string{iniPath}, std::ios::binary}; stream) {
        content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    const auto config = parse_waypoint_teleport_config(content);
    {
        const std::lock_guard lock(snapshotMutex_);
        iniPath_ = std::string{iniPath};
        config_ = config;
        lastMessage_ = "配置已加载" + std::string{content.empty() ? "（使用默认值）" : ""};
    }
    trigger_.reset();
}

auto WaypointTeleportRuntime::set_config(const WaypointTeleportConfig config) -> void {
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
            stream << serialize_waypoint_teleport_config(config);
        } else {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: failed to write waypoint teleport config '{}'\n"),
                text_encoding::widen_ascii(iniPath));
        }
    }
}

auto WaypointTeleportRuntime::tick(const float deltaSeconds,
                                   const skill_editor::WorldSessionState& session) -> void {
    static_cast<void>(deltaSeconds);
    const bool guiRequest = requestedTeleport_.exchange(false);
    if (configDirty_.exchange(false, std::memory_order_acquire)) {
        trigger_.reset();
    }
    WaypointTeleportConfig config;
    {
        const std::lock_guard lock(snapshotMutex_);
        config = config_;
    }
    const bool pressed = (GetAsyncKeyState(config.hotkeyVk) & 0x8000) != 0;
    const bool foreground = foreground_is_game();
    // 状态机每帧无条件推进，避免丢失松开的下降沿（同远程终端）。
    const bool edge = trigger_.update(std::chrono::steady_clock::now(), foreground && pressed);
    if (session.can_access_unreal()) {
        run_pending_refinement();  // 远距两段式的到期贴地（空计划为常量时间）。
    }
    const bool triggered = guiRequest || edge;
    if (!triggered) {
        return;
    }
    if (!session.can_access_unreal()) {
        note("世界尚未就绪", true);
        trigger_.end_trigger();
        return;
    }
    const auto result = execute_trigger(config);
    trigger_.end_trigger();
    static_cast<void>(result);
}

auto WaypointTeleportRuntime::request_teleport() -> void {
    requestedTeleport_.store(true);
}

auto WaypointTeleportRuntime::begin_world_transition() -> void {
    trigger_.reset();
    pendingRefinement_.active = false;
    domainDisabled_.store(false, std::memory_order_release);
    requestedTeleport_.store(false);
}

auto WaypointTeleportRuntime::finish_world_transition() -> void {}

auto WaypointTeleportRuntime::teleport_to_candidate(const WaypointTeleportConfig& config,
                                                    UObject* manager, const MarkerCandidate& target)
    -> WaypointTeleportResult {
    const auto finish = [this](const WaypointTeleportResult result, const std::string& message,
                               const bool isFailure) -> WaypointTeleportResult {
        note(message, isFailure);
        return result;
    };

    if (domainDisabled_.load(std::memory_order_acquire)) {
        return finish(WaypointTeleportResult::disabled, "结构故障已停用标记传送", true);
    }
    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    auto* const controller = pal_game::local_player_controller(worldContext);
    if (controller == nullptr) {
        return finish(WaypointTeleportResult::unavailable, "无法解析本地玩家", true);
    }
    // PlayerState 必须取控制器自身的（AController::PlayerState 属性）；FindFirstOf
    // 可能命中非本地实例（远端/模板）。
    auto* const playerStateProperty =
        CastField<FObjectPropertyBase>(controller->GetPropertyByNameInChain(STR("PlayerState")));
    auto* const playerState =
        playerStateProperty == nullptr
            ? nullptr
            : playerStateProperty->GetObjectPropertyValue(
                  playerStateProperty->ContainerPtrToValuePtr<void>(controller));
    if (!pal_game::is_valid(playerState)) {
        return finish(WaypointTeleportResult::unavailable, "无法解析本地玩家状态", true);
    }

    // 镜像远程终端的服务器同步门：标志存在且为 false 时拦截；缺失视为就绪。
    auto* const syncProperty =
        playerState->GetPropertyByNameInChain(STR("bIsCompleteSyncPlayerFromServer_InClient"));
    auto* const syncBool = CastField<FBoolProperty>(syncProperty);
    if (syncBool != nullptr && !syncBool->GetPropertyValueInContainer(playerState)) {
        return finish(WaypointTeleportResult::blocked, "世界尚未同步完成", true);
    }
    if (config.disableInDungeon &&
        pal_game::invoke<bool>(playerState, STR("IsInStage")).value_or(false)) {
        return finish(WaypointTeleportResult::blocked, "地牢内已禁用", true);
    }
    if (config.disableWhileMounted &&
        pal_game::invoke<bool>(controller, STR("IsRiding")).value_or(false)) {
        return finish(WaypointTeleportResult::blocked, "骑乘中已禁用", true);
    }
    if (config.disableDuringCombat && pal_game::player_in_battle_mode(controller)) {
        return finish(WaypointTeleportResult::blocked, "战斗中已禁用", true);
    }

    // 传送原语：K2_SetActorLocation(bSweep=false) 精确放置于玩家 Pawn——无序列
    // 状态机、无路径扫掠（前几版原语的崩溃/入地教训见函数头注释）。
    auto* const pawn = pal_game::player_pawn(controller);
    if (!pal_game::is_valid(pawn)) {
        return finish(WaypointTeleportResult::unavailable, "无法解析玩家 Pawn", true);
    }
    // 地图标记 Z 不可靠（实测会落在地表下方传送入地）：以标记 Z 为中心 ±1km 向下
    // 射线追踪取真实地面；未命中或结构不兼容时拒绝传送。
    const auto groundZ = trace_ground_z(worldContext, target.x, target.y, target.z);
    if (!groundZ.has_value()) {
        set_disabled("LineTraceSingle 签名不兼容，已停用标记传送");
        return WaypointTeleportResult::disabled;
    }
    if (!groundZ->has_value()) {
        return finish(WaypointTeleportResult::unavailable, "标记点无法探测地面（可能在水下/洞顶）",
                      true);
    }
    // 离地间隙高于玩家胶囊半高（~90cm），避免放置后胶囊与地表相交。
    constexpr double kArrivalClearance = 150.0;  // cm
    const double distanceCm = std::sqrt(target.distanceSquared);
    if (distanceCm > kDirectPlaceDistanceCm) {
        // 远距两段式：目标区块大概率未流送（实测：远距首追踪命中未加载占位高度，
        // 到达后第二次追踪才是真实地面）。第一段直接落在最佳已知高度 + 离地间隙
        // （常见情况即真实地面，零降落；估计偏差由下落起点重置兜底）。
        const double anchorZ = std::max(**groundZ, target.z);
        FVector anchor{};
        anchor.SetX(target.x);
        anchor.SetY(target.y);
        anchor.SetZ(anchorZ + kArrivalClearance + static_cast<double>(config.arrivalHeightOffset));
        const auto anchorResult = call_set_actor_location(pawn, anchor);
        if (!anchorResult.has_value()) {
            set_disabled("K2_SetActorLocation 签名不兼容，已停用标记传送");
            return WaypointTeleportResult::disabled;
        }
        if (!*anchorResult) {
            return finish(WaypointTeleportResult::unavailable, "引擎拒绝传送（目标点不可达）",
                          true);
        }
        static_cast<void>(reset_fall_origin(pawn));
        if (config.deleteMarkerAfterTeleport) {
            static_cast<void>(remove_custom_marker(manager, target.guid));
        }
        pendingRefinement_ = {.active = true,
                              .x = target.x,
                              .y = target.y,
                              .anchorZ = anchorZ,
                              .deadline = std::chrono::steady_clock::now() + kRefinementDelay};
        {
            const std::lock_guard lock(snapshotMutex_);
            teleportCount_ += 1;
        }
        return finish(WaypointTeleportResult::teleported,
                      "目标较远：已到达，正在校正地面，" +
                          std::to_string(kRefinementDelay.count() / 1000.0).substr(0, 3) +
                          " 秒后自动贴地…",
                      false);
    }

    FVector destination{};
    destination.SetX(target.x);
    destination.SetY(target.y);
    destination.SetZ(**groundZ + kArrivalClearance +
                     static_cast<double>(config.arrivalHeightOffset));
    const auto teleportResult = call_set_actor_location(pawn, destination);
    if (!teleportResult.has_value()) {
        set_disabled("K2_SetActorLocation 签名不兼容，已停用标记传送");
        return WaypointTeleportResult::disabled;
    }
    if (!*teleportResult) {
        return finish(WaypointTeleportResult::unavailable, "引擎拒绝传送（目标点不可达）", true);
    }
    static_cast<void>(reset_fall_origin(pawn));
    const bool markerRemoved =
        !config.deleteMarkerAfterTeleport || remove_custom_marker(manager, target.guid);

    {
        const std::lock_guard lock(snapshotMutex_);
        teleportCount_ += 1;
    }
    std::string message{"已传送（水平距离 "};
    message += std::to_string(static_cast<int>(distanceCm / 100.0));
    message += " 米";
    if (config.deleteMarkerAfterTeleport) {
        message += markerRemoved ? "，标记已删除" : "，标记删除失败";
    }
    message += "）";
    return finish(WaypointTeleportResult::teleported, std::move(message), false);
}

auto WaypointTeleportRuntime::execute_trigger(const WaypointTeleportConfig& config)
    -> WaypointTeleportResult {
    if (domainDisabled_.load(std::memory_order_acquire)) {
        note("结构故障已停用标记传送", true);
        return WaypointTeleportResult::disabled;
    }
    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    auto* const controller = pal_game::local_player_controller(worldContext);
    auto* const pawn = controller == nullptr ? nullptr : pal_game::player_pawn(controller);
    FVector playerLocation{};
    if (!pal_game::read_actor_location(pawn, playerLocation)) {
        note("无法读取玩家位置", true);
        return WaypointTeleportResult::unavailable;
    }
    auto* const manager = location_manager(worldContext);
    if (manager == nullptr) {
        note("无法解析位置管理器", true);
        return WaypointTeleportResult::unavailable;
    }

    std::vector<MarkerCandidate> candidates;
    if (!read_marker_candidates(manager, candidates)) {
        set_disabled("CustomMarkers 结构不兼容，已停用标记传送");
        return WaypointTeleportResult::disabled;
    }
    if (candidates.empty()) {
        note("没有自定义地图标记", true);
        return WaypointTeleportResult::noMarker;
    }
    for (auto& candidate : candidates) {
        const auto dx = candidate.x - playerLocation.X();
        const auto dy = candidate.y - playerLocation.Y();
        candidate.distanceSquared = dx * dx + dy * dy;
    }
    const auto nearest = select_nearest_marker(candidates);
    if (!nearest.has_value()) {
        note("没有自定义地图标记", true);
        return WaypointTeleportResult::noMarker;
    }
    return teleport_to_candidate(config, manager, candidates[*nearest]);
}

auto WaypointTeleportRuntime::run_pending_refinement() -> void {
    if (!pendingRefinement_.active) {
        return;
    }
    if (std::chrono::steady_clock::now() < pendingRefinement_.deadline) {
        return;
    }
    pendingRefinement_.active = false;

    // best-effort：任何一步失败都只放弃本次校正（玩家保持空投点下落状态，可滑翔）。
    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    auto* const controller = pal_game::local_player_controller(worldContext);
    auto* const pawn = controller == nullptr ? nullptr : pal_game::player_pawn(controller);
    if (!pal_game::is_valid(pawn)) {
        return;
    }
    const auto groundZ = trace_ground_z(worldContext, pendingRefinement_.x, pendingRefinement_.y,
                                        pendingRefinement_.anchorZ);
    if (!groundZ.has_value() || !groundZ->has_value()) {
        note("贴地校正失败：无法探测地面", true);
        return;
    }
    const double correctedZ = **groundZ + 150.0;
    if (std::abs(correctedZ - pendingRefinement_.anchorZ) < kRefinementMinDeltaCm) {
        return;  // 首次追踪已正确（或玩家已自行落地），无需二次放置。
    }
    FVector destination{};
    destination.SetX(pendingRefinement_.x);
    destination.SetY(pendingRefinement_.y);
    destination.SetZ(correctedZ);
    if (call_set_actor_location(pawn, destination).value_or(false)) {
        static_cast<void>(reset_fall_origin(pawn));
        note("已自动贴地", false);
    }
}

auto WaypointTeleportRuntime::set_disabled(const std::string& message) -> void {
    domainDisabled_.store(true, std::memory_order_release);
    note(message, true);
}

auto WaypointTeleportRuntime::note(const std::string& message, const bool isFailure) -> void {
    const std::lock_guard lock(snapshotMutex_);
    lastMessage_ = message;
    if (isFailure) {
        failCount_ += 1;
    }
}

auto WaypointTeleportRuntime::snapshot() const -> WaypointTeleportSnapshot {
    const std::lock_guard lock(snapshotMutex_);
    return {.config = config_,
            .domainDisabled = domainDisabled_.load(std::memory_order_acquire),
            .lastMessage = lastMessage_,
            .teleportCount = teleportCount_,
            .failCount = failCount_};
}

}  // namespace waypoint_teleport
