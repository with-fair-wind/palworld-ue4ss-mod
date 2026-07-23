#pragma once

#include "skill_editor_service.hpp"

namespace pal_skills
{
class PalSkillGateway final : public skill_editor::ISkillGateway
{
public:
    [[nodiscard]] auto is_valid(skill_editor::SkillTarget target) const -> bool override;
    auto read_state(skill_editor::SkillTarget target) -> skill_editor::SkillState override;
    auto add_passive(skill_editor::SkillTarget target, std::string_view id) -> bool override;
    auto remove_passive(skill_editor::SkillTarget target, std::string_view id) -> bool override;
    auto rewrite_active(
        skill_editor::SkillTarget target,
        std::span<const skill_editor::ActiveSkill> skills) -> bool override;
};
}
