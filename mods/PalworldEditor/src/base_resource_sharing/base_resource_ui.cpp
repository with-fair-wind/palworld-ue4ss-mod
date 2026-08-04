/**
 * @file base_resource_ui.cpp
 * @brief 据点资源共享与爪钩枪无冷却开关的 ImGui 渲染实现。
 * @details 只在 GUI 线程调用；切换开关只提交进程内原子请求，不直接访问 Unreal 对象。
 *          实现从 src/mod/dllmain.cpp 拆出，签名不变。
 */
#include <mutex>
#include <string>

#include <imgui.h>
#include <mod/mod_core.hpp>

void PalworldEditorMod::render_base_resource_sharing(PalworldEditorMod* self) {
    if (!ImGui::CollapsingHeader("据点资源共享")) {
        return;
    }

    const auto snapshot = self->baseResourceBridge_.snapshot();
    bool enabled = self->requestedBaseSharingEnabled_.load(std::memory_order_acquire);
    if (ImGui::Checkbox("同公会跨据点资源共享", &enabled)) {
        self->requestedBaseSharingEnabled_.store(enabled, std::memory_order_release);
        self->baseSharingSettingDirty_.store(true, std::memory_order_release);
    }

    ImGui::TextWrapped("%s", snapshot.status.c_str());
    const auto phaseLabel = [&snapshot]() -> const char* {
        switch (snapshot.persistentPhase) {
            case base_resource_sharing::PersistentUnionPhase::off:
                return "关闭";
            case base_resource_sharing::PersistentUnionPhase::initializing:
                return "初始化";
            case base_resource_sharing::PersistentUnionPhase::ready:
                return "就绪";
            case base_resource_sharing::PersistentUnionPhase::reconciling:
                return "校准";
            case base_resource_sharing::PersistentUnionPhase::restoring:
                return "恢复";
            case base_resource_sharing::PersistentUnionPhase::failed:
                return "安全停用";
        }
        return "未知";
    }();
    const auto surfaceLabel = [&snapshot]() -> const char* {
        switch (snapshot.consumerSurface) {
            case base_resource_sharing::ResourceConsumerSurface::none:
                return "无";
            case base_resource_sharing::ResourceConsumerSurface::playerHelper:
                return "玩家主背包 Helper";
            case base_resource_sharing::ResourceConsumerSurface::currentBaseModule:
                return "当前据点仓储模块";
            case base_resource_sharing::ResourceConsumerSurface::guildBaseModules:
                return "同公会据点仓储图";
        }
        return "未知";
    }();
    ImGui::TextDisabled("持久联合：%s；消费入口：%s", phaseLabel, surfaceLabel);
    ImGui::TextDisabled("原生登记边（已应用/待处理）：%zu / %zu", snapshot.appliedEdgeCount,
                        snapshot.pendingEdgeCount);
    ImGui::TextDisabled("可用/待加载容器：%zu / %zu", snapshot.containerCount,
                        snapshot.pendingContainerCount);
    ImGui::TextDisabled("目录耗时（最近/最近成功/本世界峰值）：%.2f / %.2f / %.2f ms；尝试：%zu 次",
                        snapshot.lastCatalogMilliseconds,
                        snapshot.lastSuccessfulCatalogMilliseconds,
                        snapshot.maximumCatalogMilliseconds, snapshot.catalogAttemptCount);
    if (snapshot.safetyDisabled) {
        ImGui::TextColored(
            ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
            "检测到联合序列或恢复异常；相关能力已在本世界安全停用，切换开关不会绕过。");
    }
    ImGui::TextDisabled("仅本次游戏进程有效；重新启动游戏后默认关闭。");
    ImGui::TextDisabled("仅支持单人世界/本地房主；只影响制作和建造材料消耗，不合并箱子界面。");
}

void PalworldEditorMod::render_grapple_no_cooldown(PalworldEditorMod* self) {
    bool enabled = self->requestedGrappleNoCooldown_.load(std::memory_order_acquire);
    const auto phase = self->grappleRuntimePhase_.load(std::memory_order_acquire);
    const auto safetyDisabled = self->grappleSafetyDisabled_.load(std::memory_order_acquire) ||
                                phase == grappling_hook::CooldownRuntimePhase::safetyDisabled;
    ImGui::BeginDisabled(safetyDisabled);
    if (ImGui::Checkbox("爪钩枪无冷却", &enabled)) {
        self->requestedGrappleNoCooldown_.store(enabled, std::memory_order_release);
        self->grappleSettingDirty_.store(true, std::memory_order_release);
    }
    ImGui::EndDisabled();
    if (phase == grappling_hook::CooldownRuntimePhase::waitingForRetry &&
        ImGui::Button("重新检测爪钩枪")) {
        self->grappleRetryRequested_.store(true, std::memory_order_release);
    }
    std::string runtimeStatus;
    {
        const std::lock_guard lock(self->grappleStatusMutex_);
        runtimeStatus = self->grappleRuntimeStatus_;
    }
    if (!runtimeStatus.empty()) {
        ImGui::TextWrapped("%s", runtimeStatus.c_str());
    }
    if (phase == grappling_hook::CooldownRuntimePhase::waitingForRetry) {
        ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.2F, 1.0F),
                           "当前未找到已加载的正式爪钩枪；装备后请点击“重新检测爪钩枪”。");
    }
    if (safetyDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "爪钩字段布局、写入验证或恢复失败；已安全停用，切换开关不会绕过。");
    }
    ImGui::TextDisabled("仅本次游戏进程有效；重新启动游戏后默认关闭。");
    ImGui::TextDisabled(
        "只修改物品 ID 可确认的正式爪钩枪；关闭和切图时恢复原值。热卸载前请先关闭。");
}
