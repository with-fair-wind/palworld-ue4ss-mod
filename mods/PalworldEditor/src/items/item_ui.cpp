/**
 * @file item_ui.cpp
 * @brief 物品给予、物品目录浏览和主背包快照的 ImGui 渲染实现。
 * @details 只在 GUI 线程调用；控件只读写 PalworldEditorMod 的值缓存与原子请求标志，
 *          不直接访问 Unreal 对象。实现从 src/mod/dllmain.cpp 拆出，签名不变。
 */
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

#include <imgui.h>
#include <mod/editor_ui.hpp>
#include <mod/mod_core.hpp>

namespace {
[[nodiscard]] auto item_label_reference(const item_catalog::ItemCatalogSnapshot& catalog,
                                        const std::string& id) -> const std::string& {
    const auto found = catalog.labelsById.find(id);
    return found == catalog.labelsById.end() ? id : found->second;
}

[[nodiscard]] auto stack_limit_phase_label(
    const item_stack_limit::StackLimitRuntimePhase phase) noexcept -> const char* {
    using enum item_stack_limit::StackLimitRuntimePhase;
    switch (phase) {
        case off:
            return "已关闭";
        case readyToApply:
            return "等待应用";
        case applying:
            return "正在应用";
        case active:
            return "已生效";
        case waitingForRetry:
            return "未找到目标；等待手动重试";
        case restoring:
            return "正在恢复或保留恢复责任";
        case safetyDisabled:
            return "安全停用";
    }
    return "未知";
}
}  // namespace

auto PalworldEditorMod::clamp(int v, int lo, int hi) -> int {
    return v < lo ? lo : (v > hi ? hi : v);
}

void PalworldEditorMod::render_give_items(PalworldEditorMod* self) {
    editor_ui::section_header("给予物品");
    ImGui::SetNextItemWidth(200.0F);
    ImGui::InputText("物品 ID", self->item_buf_, sizeof(self->item_buf_));
    ImGui::InputInt("数量", &self->count_input_);
    self->count_input_ = clamp(self->count_input_, 1, 9999);
    {
        editor_ui::scoped_accent_button accent;
        if (ImGui::Button("给予")) {
            const std::lock_guard lock(self->req_mutex_);
            self->give_item_ = self->item_buf_;
            self->give_count_ = self->count_input_;
            self->give_requested_ = true;
        }
    }
}

void PalworldEditorMod::render_item_browser(PalworldEditorMod* self) {
    editor_ui::section_header("物品目录");
    if (ImGui::Button("扫描游戏物品")) {
        self->want_scan_items_.store(true);
    }
    ImGui::SameLine();
    ImGui::InputText("##search", self->search_buf_, sizeof(self->search_buf_));
    {
        const std::lock_guard lock(self->inv_mutex_);
        ImGui::TextDisabled("（%d 件物品）", static_cast<int>(self->item_db_cache_.items.size()));
    }
    ImGui::BeginChild("browser", ImVec2(380, 160), true);
    {
        const std::lock_guard lock(self->inv_mutex_);
        if (self->item_db_cache_.items.empty()) {
            ImGui::TextDisabled("尚未发现物品，请重新扫描。");
        }
        const auto visible = item_catalog::filter_items(self->item_db_cache_, self->search_buf_);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const auto* const item = visible[static_cast<std::size_t>(index)];
                const auto& label = item_label_reference(self->item_db_cache_, item->id);
                if (ImGui::Selectable(label.c_str())) {
                    const auto copyLen = std::min(item->id.size(), sizeof(self->item_buf_) - 1);
                    std::memcpy(self->item_buf_, item->id.data(), copyLen);
                    self->item_buf_[copyLen] = '\0';
                }
            }
        }
    }
    ImGui::EndChild();
}

void PalworldEditorMod::render_inventory(PalworldEditorMod* self) {
    editor_ui::section_header("背包");
    const bool writesDisabled = self->inventoryWritesDisabled_.load(std::memory_order_acquire);
    if (ImGui::Button("刷新背包")) {
        self->want_read_.store(true);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("（点击物品选中，再设置数量）");
    {
        const std::lock_guard lock(self->inv_mutex_);
        ImGui::BeginChild("invlist", ImVec2(380, 220), true);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(self->inv_cache_.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const auto& entry = self->inv_cache_[static_cast<std::size_t>(index)];
                const auto& itemLabel = item_label_reference(self->item_db_cache_, entry.item_id);
                const auto label = itemLabel + "  x" + std::to_string(entry.count) + " ##inv" +
                                   std::to_string(index);
                if (ImGui::Selectable(label.c_str(), self->selected_ == index)) {
                    self->selected_ = index;
                    self->set_count_input_ = entry.count;
                }
            }
        }
        ImGui::EndChild();

        if (self->selected_ >= 0 && self->selected_ < static_cast<int>(self->inv_cache_.size())) {
            const auto& e = self->inv_cache_[self->selected_];
            const auto& itemLabel = item_label_reference(self->item_db_cache_, e.item_id);
            ImGui::Text("已选中：%s（槽位 %d，×%d）", itemLabel.c_str(),
                        static_cast<int>(e.slot_index), e.count);
            ImGui::BeginDisabled(writesDisabled);
            ImGui::InputInt("新数量", &self->set_count_input_);
            self->set_count_input_ = clamp(self->set_count_input_, 0, 9999);
            {
                editor_ui::scoped_accent_button accent;
                if (ImGui::Button("设置数量")) {
                    const std::lock_guard lock2(self->req_mutex_);
                    self->modify_slot_ = e.slot_index;
                    self->modify_count_ = self->set_count_input_;
                    self->modify_requested_ = true;
                }
            }
            ImGui::EndDisabled();
        }
    }
    if (writesDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "背包数量恢复验证失败；本世界已停用后续直接写入。");
    }
}

void PalworldEditorMod::render_stack_unlimited(PalworldEditorMod* self) {
    editor_ui::section_header("物品堆叠");
    const auto phase = self->stack_limit_phase_.load(std::memory_order_acquire);
    const bool mutationsDisabled =
        phase == item_stack_limit::StackLimitRuntimePhase::applying ||
        phase == item_stack_limit::StackLimitRuntimePhase::restoring ||
        phase == item_stack_limit::StackLimitRuntimePhase::safetyDisabled;
    bool unlimited = self->requested_stack_unlimited_.load(std::memory_order_acquire);
    ImGui::BeginDisabled(mutationsDisabled);
    if (ImGui::Checkbox("普通物品高堆叠上限（999,999,999）", &unlimited)) {
        self->requested_stack_unlimited_.store(unlimited, std::memory_order_release);
        self->stack_setting_dirty_.store(true, std::memory_order_release);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("状态：%s", stack_limit_phase_label(phase));
    ImGui::TextDisabled("仅修改原始上限为 9999 的普通物品；装备、饰品等特殊物品保持原值。");
    ImGui::TextDisabled("仅本次游戏进程有效；关闭开关或切换世界时按对象恢复原值。");

    std::string status;
    {
        const std::lock_guard lock(self->stack_limit_status_mutex_);
        status = self->stack_limit_status_;
    }
    if (!status.empty()) {
        ImGui::TextWrapped("%s", status.c_str());
    }
}
