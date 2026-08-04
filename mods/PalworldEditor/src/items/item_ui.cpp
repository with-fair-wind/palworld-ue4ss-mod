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
        for (const auto* item : visible) {
            const auto label = item_catalog::item_label(*item);
            if (ImGui::Selectable(label.c_str())) {
                const auto copyLen = std::min(item->id.size(), sizeof(self->item_buf_) - 1);
                std::memcpy(self->item_buf_, item->id.data(), copyLen);
                self->item_buf_[copyLen] = '\0';
            }
        }
    }
    ImGui::EndChild();
}

void PalworldEditorMod::render_inventory(PalworldEditorMod* self) {
    editor_ui::section_header("背包");
    if (ImGui::Button("刷新背包")) {
        self->want_read_.store(true);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("（点击物品选中，再设置数量）");
    {
        const std::lock_guard lock(self->inv_mutex_);
        ImGui::BeginChild("invlist", ImVec2(380, 220), true);
        for (int i = 0; i < static_cast<int>(self->inv_cache_.size()); ++i) {
            const auto& e = self->inv_cache_[i];
            const auto itemLabel = item_catalog::item_label(self->item_db_cache_, e.item_id);
            const auto label =
                itemLabel + "  x" + std::to_string(e.count) + " ##inv" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), self->selected_ == i)) {
                self->selected_ = i;
                self->set_count_input_ = e.count;
            }
        }
        ImGui::EndChild();

        if (self->selected_ >= 0 && self->selected_ < static_cast<int>(self->inv_cache_.size())) {
            const auto& e = self->inv_cache_[self->selected_];
            const auto itemLabel = item_catalog::item_label(self->item_db_cache_, e.item_id);
            ImGui::Text("已选中：%s（槽位 %d，×%d）", itemLabel.c_str(),
                        static_cast<int>(e.slot_index), e.count);
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
        }
    }
}
