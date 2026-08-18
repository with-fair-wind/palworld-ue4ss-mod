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
        FVector location{};
        locationProperty->CopyCompleteValue(&location, value);
        output.push_back(MarkerCandidate{.x = location.X(), .y = location.Y(), .z = location.Z()});
    }
    return true;
}

/** @brief 调用 UPalSyncTeleportComponent:SyncTeleport；结构不兼容返回 false。 */
[[nodiscard]] auto call_sync_teleport(UObject* component, const FVector& destination) -> bool {
    auto* const function = pal_game::is_valid(component)
                               ? component->GetFunctionByNameInChain(STR("SyncTeleport"))
                               : nullptr;
    auto* const parameter = function == nullptr ? nullptr
                                                : CastField<FStructProperty>(function->FindProperty(
                                                      FName(STR("Parameter"), FNAME_Find)));
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_input_parameter(parameter) || function->GetReturnProperty() != nullptr) {
        return false;
    }
    auto* const parameterStruct = parameter->GetStruct().Get();
    auto* const locationProperty = parameterStruct == nullptr
                                       ? nullptr
                                       : CastField<FStructProperty>(parameterStruct->FindProperty(
                                             FName(STR("Location"), FNAME_Find)));
    auto* const rotationProperty = parameterStruct == nullptr
                                       ? nullptr
                                       : CastField<FStructProperty>(parameterStruct->FindProperty(
                                             FName(STR("Rotation"), FNAME_Find)));
    if (locationProperty == nullptr || rotationProperty == nullptr ||
        locationProperty->GetElementSize() != sizeof(FVector)) {
        return false;
    }
    // 三项跳过标志（音效/淡入淡出）与音效对象保持 InitializeStruct 的零值。

    pal_game::FunctionParams params{function};
    locationProperty->CopyCompleteValue(parameter->ContainerPtrToValuePtr<void>(params.data()),
                                        &destination);
    // 单位四元数按引擎宽度写入：UE5 为 4×double（0x20），UE4 为 4×float（0x10）。
    auto* const rotationSlot = rotationProperty->ContainerPtrToValuePtr<void>(params.data());
    if (rotationProperty->GetElementSize() == sizeof(double) * 4) {
        const double identity[]{0.0, 0.0, 0.0, 1.0};
        std::memcpy(rotationSlot, identity, sizeof(identity));
    } else if (rotationProperty->GetElementSize() == sizeof(float) * 4) {
        const float identity[]{0.0F, 0.0F, 0.0F, 1.0F};
        std::memcpy(rotationSlot, identity, sizeof(identity));
    } else {
        return false;
    }
    component->ProcessEvent(function, params.data());
    return true;
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
    domainDisabled_.store(false, std::memory_order_release);
    requestedTeleport_.store(false);
}

auto WaypointTeleportRuntime::finish_world_transition() -> void {}

auto WaypointTeleportRuntime::execute_trigger(const WaypointTeleportConfig& config)
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
    auto* const playerState = UObjectGlobals::FindFirstOf(STR("PalPlayerState"));
    if (controller == nullptr || playerState == nullptr) {
        return finish(WaypointTeleportResult::unavailable, "无法解析本地玩家", true);
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

    FVector playerLocation{};
    if (!pal_game::read_actor_location(pal_game::player_pawn(controller), playerLocation)) {
        return finish(WaypointTeleportResult::unavailable, "无法读取玩家位置", true);
    }
    auto* const manager = location_manager(worldContext);
    if (manager == nullptr) {
        return finish(WaypointTeleportResult::unavailable, "无法解析位置管理器", true);
    }

    std::vector<MarkerCandidate> candidates;
    if (!read_marker_candidates(manager, candidates)) {
        set_disabled("CustomMarkers 结构不兼容，已停用标记传送");
        return WaypointTeleportResult::disabled;
    }
    if (candidates.empty()) {
        return finish(WaypointTeleportResult::noMarker, "没有自定义地图标记", true);
    }
    for (auto& candidate : candidates) {
        const auto dx = candidate.x - playerLocation.X();
        const auto dy = candidate.y - playerLocation.Y();
        candidate.distanceSquared = dx * dx + dy * dy;
    }
    const auto nearest = select_nearest_marker(candidates);
    if (!nearest.has_value()) {
        return finish(WaypointTeleportResult::noMarker, "没有自定义地图标记", true);
    }

    auto* const component =
        pal_game::invoke<UObject*>(playerState, STR("GetSyncTeleportComp")).value_or(nullptr);
    if (!pal_game::is_valid(component)) {
        return finish(WaypointTeleportResult::unavailable, "无法解析传送组件", true);
    }
    // 到达点 = 标记水平位置 + 标记 Z + 配置偏移：以标记点地形高度为锚，默认 +100m
    // 便于滑翔落地；偏移可在 ini 调整（0 = 直接落在标记点）。
    FVector destination{};
    destination.SetX(candidates[*nearest].x);
    destination.SetY(candidates[*nearest].y);
    destination.SetZ(candidates[*nearest].z + static_cast<double>(config.arrivalHeightOffset));
    if (!call_sync_teleport(component, destination)) {
        set_disabled("SyncTeleport 签名不兼容，已停用标记传送");
        return WaypointTeleportResult::disabled;
    }

    {
        const std::lock_guard lock(snapshotMutex_);
        teleportCount_ += 1;
    }
    return finish(WaypointTeleportResult::teleported,
                  "已传送到最近的地图标记（水平距离 " +
                      std::to_string(static_cast<int>(
                          std::sqrt(candidates[*nearest].distanceSquared) / 100.0)) +
                      " 米）",
                  false);
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
