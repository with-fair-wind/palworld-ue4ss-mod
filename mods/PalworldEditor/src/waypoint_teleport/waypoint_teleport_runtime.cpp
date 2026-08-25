/**
 * @file waypoint_teleport_runtime.cpp
 * @brief 实现传送至最近地图标记点：门控、CustomMarkers 读取与无扫掠放置。
 * @details 复用 pal_game 公共原语（控制器/Pawn/战斗判定/按键状态机）；标记 TMap 与
 *          传送参数全部经 FProperty API 按名读写，不手写参数布局。跨帧只保存纯值。
 */
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <common/game_foreground.hpp>
#include <common/game_reflection.hpp>
#include <common/player_state_gate.hpp>
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
/** @brief 到达点离地间隙（cm）：高于玩家胶囊半高（~90cm），避免放置后胶囊与地表相交。 */
inline constexpr double kArrivalClearanceCm{150.0};
inline constexpr std::size_t kPalworld10HitResultSize{0xE8};
inline constexpr std::size_t kLinearColorSize{0x10};

/**
 * @brief 校验精确 FVector 属性。
 */
[[nodiscard]] auto matches_vector_property(FStructProperty* property) -> bool {
    return pal_game::matches_struct_identity(property, STR("Vector"), sizeof(FVector));
}

[[nodiscard]] auto matches_trace_impact_property(FStructProperty* property) -> bool {
    return matches_vector_property(property) ||
           pal_game::matches_struct_identity(property, STR("Vector_NetQuantize"), sizeof(FVector));
}

struct LineTraceContract {
    FObjectPropertyBase* worldContextObject{};
    FStructProperty* start{};
    FStructProperty* end{};
    FByteProperty* traceChannel{};
    FBoolProperty* traceComplex{};
    FArrayProperty* actorsToIgnore{};
    FByteProperty* drawDebugType{};
    FStructProperty* outHit{};
    FBoolProperty* ignoreSelf{};
    FStructProperty* traceColor{};
    FStructProperty* traceHitColor{};
    FFloatProperty* drawTime{};
    FBoolProperty* returnValue{};
    FStructProperty* impactPoint{};
};

[[nodiscard]] auto resolve_line_trace_contract(UFunction* function)
    -> std::optional<LineTraceContract> {
    if (!pal_game::has_exact_parameter_count(function, 13)) {
        return std::nullopt;
    }
    auto* const worldContextObject = CastField<FObjectPropertyBase>(
        function->FindProperty(FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const start =
        CastField<FStructProperty>(function->FindProperty(FName(STR("Start"), FNAME_Find)));
    auto* const end =
        CastField<FStructProperty>(function->FindProperty(FName(STR("End"), FNAME_Find)));
    auto* const traceChannel =
        CastField<FByteProperty>(function->FindProperty(FName(STR("TraceChannel"), FNAME_Find)));
    auto* const traceComplex =
        CastField<FBoolProperty>(function->FindProperty(FName(STR("bTraceComplex"), FNAME_Find)));
    auto* const actorsToIgnore =
        CastField<FArrayProperty>(function->FindProperty(FName(STR("ActorsToIgnore"), FNAME_Find)));
    auto* const ignoredActor = actorsToIgnore == nullptr
                                   ? nullptr
                                   : CastField<FObjectPropertyBase>(actorsToIgnore->GetInner());
    auto* const drawDebugType =
        CastField<FByteProperty>(function->FindProperty(FName(STR("DrawDebugType"), FNAME_Find)));
    auto* const outHit =
        CastField<FStructProperty>(function->FindProperty(FName(STR("OutHit"), FNAME_Find)));
    auto* const ignoreSelf =
        CastField<FBoolProperty>(function->FindProperty(FName(STR("bIgnoreSelf"), FNAME_Find)));
    auto* const traceColor =
        CastField<FStructProperty>(function->FindProperty(FName(STR("TraceColor"), FNAME_Find)));
    auto* const traceHitColor =
        CastField<FStructProperty>(function->FindProperty(FName(STR("TraceHitColor"), FNAME_Find)));
    auto* const drawTime =
        CastField<FFloatProperty>(function->FindProperty(FName(STR("DrawTime"), FNAME_Find)));
    auto* const returnValue =
        CastField<FBoolProperty>(function->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
    auto* const hitStruct = outHit == nullptr ? nullptr : outHit->GetStruct().Get();
    auto* const impactPoint =
        hitStruct == nullptr ? nullptr
                             : CastField<FStructProperty>(
                                   hitStruct->FindProperty(FName(STR("ImpactPoint"), FNAME_Find)));
    if (!pal_game::is_input_parameter(worldContextObject) || !pal_game::is_input_parameter(start) ||
        !matches_vector_property(start) || !pal_game::is_input_parameter(end) ||
        !matches_vector_property(end) || !pal_game::is_input_parameter(traceChannel) ||
        !pal_game::is_input_parameter(traceComplex) ||
        !pal_game::is_input_parameter(actorsToIgnore) || ignoredActor == nullptr ||
        !!(actorsToIgnore->GetArrayFlags() & EArrayPropertyFlags::UsesMemoryImageAllocator) ||
        !pal_game::is_input_parameter(drawDebugType) || !pal_game::is_output_parameter(outHit) ||
        !pal_game::matches_struct_identity(outHit, STR("HitResult"), kPalworld10HitResultSize) ||
        !pal_game::is_input_parameter(ignoreSelf) || !pal_game::is_input_parameter(traceColor) ||
        !pal_game::matches_struct_identity(traceColor, STR("LinearColor"), kLinearColorSize) ||
        !pal_game::is_input_parameter(traceHitColor) ||
        !pal_game::matches_struct_identity(traceHitColor, STR("LinearColor"), kLinearColorSize) ||
        !pal_game::is_input_parameter(drawTime) || !pal_game::is_return_parameter(returnValue) ||
        !matches_trace_impact_property(impactPoint)) {
        return std::nullopt;
    }
    return LineTraceContract{.worldContextObject = worldContextObject,
                             .start = start,
                             .end = end,
                             .traceChannel = traceChannel,
                             .traceComplex = traceComplex,
                             .actorsToIgnore = actorsToIgnore,
                             .drawDebugType = drawDebugType,
                             .outHit = outHit,
                             .ignoreSelf = ignoreSelf,
                             .traceColor = traceColor,
                             .traceHitColor = traceHitColor,
                             .drawTime = drawTime,
                             .returnValue = returnValue,
                             .impactPoint = impactPoint};
}

struct SetActorLocationContract {
    FStructProperty* newLocation{};
    FBoolProperty* sweep{};
    FStructProperty* sweepHitResult{};
    FBoolProperty* teleport{};
    FBoolProperty* returnValue{};
};

[[nodiscard]] auto resolve_set_actor_location_contract(UFunction* function)
    -> std::optional<SetActorLocationContract> {
    auto* const newLocation =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("NewLocation"), FNAME_Find)));
    auto* const sweep =
        function == nullptr
            ? nullptr
            : CastField<FBoolProperty>(function->FindProperty(FName(STR("bSweep"), FNAME_Find)));
    auto* const sweepHitResult =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("SweepHitResult"), FNAME_Find)));
    auto* const teleport =
        function == nullptr
            ? nullptr
            : CastField<FBoolProperty>(function->FindProperty(FName(STR("bTeleport"), FNAME_Find)));
    auto* const returnValue = function == nullptr ? nullptr
                                                  : CastField<FBoolProperty>(function->FindProperty(
                                                        FName(STR("ReturnValue"), FNAME_Find)));
    if (!pal_game::has_exact_parameter_count(function, 5) ||
        !pal_game::is_input_parameter(newLocation) || !matches_vector_property(newLocation) ||
        !pal_game::is_input_parameter(sweep) || !pal_game::is_output_parameter(sweepHitResult) ||
        !pal_game::matches_struct_identity(sweepHitResult, STR("HitResult"),
                                           kPalworld10HitResultSize) ||
        !pal_game::is_input_parameter(teleport) || !pal_game::is_return_parameter(returnValue)) {
        return std::nullopt;
    }
    return SetActorLocationContract{.newLocation = newLocation,
                                    .sweep = sweep,
                                    .sweepHitResult = sweepHitResult,
                                    .teleport = teleport,
                                    .returnValue = returnValue};
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
 *          IconLocation 按名解析。结构不兼容返回 false（调用方安全停用）。
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
        map == nullptr ||
        !pal_game::matches_struct_identity(keyProperty, STR("Guid"), sizeof(FGuid))) {
        return false;
    }
    auto* const valueStruct = valueProperty->GetStruct().Get();
    auto* const locationProperty = valueStruct == nullptr
                                       ? nullptr
                                       : CastField<FStructProperty>(valueStruct->FindProperty(
                                             FName(STR("IconLocation"), FNAME_Find)));
    if (!pal_game::matches_struct_identity(locationProperty, STR("Vector"), sizeof(FVector))) {
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
    const auto contract = resolve_line_trace_contract(function);
    if (!pal_game::is_valid(library) || !contract.has_value()) {
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
    contract->worldContextObject->SetObjectPropertyValue(
        contract->worldContextObject->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    contract->start->CopyCompleteValue(contract->start->ContainerPtrToValuePtr<void>(params.data()),
                                       &start);
    contract->end->CopyCompleteValue(contract->end->ContainerPtrToValuePtr<void>(params.data()),
                                     &end);
    contract->traceChannel->SetPropertyValueInContainer(
        params.data(), static_cast<std::uint8_t>(0));  // 通道 0（PalSquad 实证）
    contract->traceComplex->SetPropertyValueInContainer(params.data(), false);
    contract->drawDebugType->SetPropertyValueInContainer(params.data(),
                                                         static_cast<std::uint8_t>(0));
    contract->ignoreSelf->SetPropertyValueInContainer(params.data(), true);
    contract->drawTime->SetPropertyValueInContainer(params.data(), 0.0F);
    library->ProcessEvent(function, params.data());
    if (!contract->returnValue->GetPropertyValueInContainer(params.data())) {
        return std::optional<double>{};  // 未命中：空 optional 内层
    }
    // ImpactPoint 是 FHitResult 内部字段：容器必须是 OutHit 在参数缓冲中的实例，
    // 而不是参数缓冲本身（否则读到 Start/End 区域的错位数据）。
    void* const hitInstance = contract->outHit->ContainerPtrToValuePtr<void>(params.data());
    FVector impact{};
    contract->impactPoint->CopyCompleteValue(
        &impact, contract->impactPoint->ContainerPtrToValuePtr<void>(hitInstance));
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
    const auto contract = resolve_set_actor_location_contract(function);
    if (!contract.has_value()) {
        return std::nullopt;
    }

    pal_game::FunctionParams params{function};
    contract->newLocation->CopyCompleteValue(
        contract->newLocation->ContainerPtrToValuePtr<void>(params.data()), &destination);
    contract->sweep->SetPropertyValueInContainer(params.data(), false);
    contract->teleport->SetPropertyValueInContainer(params.data(), true);
    pawn->ProcessEvent(function, params.data());
    return contract->returnValue->GetPropertyValueInContainer(params.data());
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
    const bool foreground = pal_game::foreground_is_game();
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
                                                    UObject* worldContext, UObject* controller,
                                                    UObject* pawn, UObject* manager,
                                                    const MarkerCandidate& target)
    -> WaypointTeleportResult {
    const auto finish = [this](const WaypointTeleportResult result, const std::string& message,
                               const bool isFailure) -> WaypointTeleportResult {
        note(message, isFailure);
        return result;
    };

    if (domainDisabled_.load(std::memory_order_acquire)) {
        return finish(WaypointTeleportResult::disabled, "结构故障已停用标记传送", true);
    }
    // 句柄由 execute_trigger 同帧解析传入；此处只做有效性复核，不重复解析。
    if (!pal_game::is_valid(controller) || !pal_game::is_valid(pawn)) {
        return finish(WaypointTeleportResult::unavailable, "无法解析本地玩家或 Pawn", true);
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
    if (config.disableInDungeon) {
        const auto inStage = pal_game::invoke<bool>(playerState, STR("IsInStage"));
        if (!pal_game::state_gate_allows(inStage)) {
            return finish(WaypointTeleportResult::blocked,
                          inStage.has_value() ? "地牢内已禁用" : "地牢状态不可读，已拦截", true);
        }
    }
    if (config.disableWhileMounted) {
        const auto riding = pal_game::invoke<bool>(controller, STR("IsRiding"));
        if (!pal_game::state_gate_allows(riding)) {
            return finish(WaypointTeleportResult::blocked,
                          riding.has_value() ? "骑乘中已禁用" : "骑乘状态不可读，已拦截", true);
        }
    }
    if (config.disableDuringCombat) {
        const auto battle = pal_game::player_in_battle_mode(controller);
        if (!pal_game::state_gate_allows(battle)) {
            return finish(WaypointTeleportResult::blocked,
                          battle.has_value() ? "战斗中已禁用" : "战斗状态不可读，已拦截", true);
        }
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
    const double distanceCm = std::sqrt(target.distanceSquared);
    if (distanceCm > kDirectPlaceDistanceCm) {
        // 远距两段式：目标区块大概率未流送（实测：远距首追踪命中未加载占位高度，
        // 到达后第二次追踪才是真实地面）。第一段直接落在最佳已知高度 + 离地间隙
        // （常见情况即真实地面，零降落；估计偏差由下落起点重置兜底）。
        const double anchorZ = std::max(**groundZ, target.z);
        FVector anchor{};
        anchor.SetX(target.x);
        anchor.SetY(target.y);
        anchor.SetZ(anchorZ + kArrivalClearanceCm +
                    static_cast<double>(config.arrivalHeightOffset));
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
        pendingRefinement_ = {.active = true,
                              .x = target.x,
                              .y = target.y,
                              .anchorZ = anchorZ,
                              .arrivalHeightOffset = config.arrivalHeightOffset,
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
    destination.SetZ(**groundZ + kArrivalClearanceCm +
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

    {
        const std::lock_guard lock(snapshotMutex_);
        teleportCount_ += 1;
    }
    std::string message{"已传送（水平距离 "};
    message += std::to_string(static_cast<int>(distanceCm / 100.0));
    message += " 米";
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
    return teleport_to_candidate(config, worldContext, controller, pawn, manager,
                                 candidates[*nearest]);
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
    // 校正目标与比较基线都必须带上第一段使用的离地偏移：偏移丢失会把配置了
    // ArrivalHeightOffset 的玩家在校正时拉回默认间隙；基线若不含间隙/偏移，
    // "首次追踪即正确"的场景差值恒为正的间隙量，永远无法跳过二次放置。
    const double offset = static_cast<double>(pendingRefinement_.arrivalHeightOffset);
    const double correctedZ = **groundZ + kArrivalClearanceCm + offset;
    const double firstPlacedZ = pendingRefinement_.anchorZ + kArrivalClearanceCm + offset;
    if (std::abs(correctedZ - firstPlacedZ) < kRefinementMinDeltaCm) {
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
