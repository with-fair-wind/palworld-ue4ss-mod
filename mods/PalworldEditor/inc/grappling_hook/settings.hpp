/**
 * @file settings.hpp
 * @brief 爪钩枪功能的独立持久化配置值。
 */
#pragma once

/** @brief 提供爪钩枪功能配置。 */
namespace grappling_hook {

/** @brief `[GrapplingHook]` 配置节；所有选项默认失败安全地关闭。 */
struct Settings {
    bool noCooldown{}; /**< 是否期望启用爪钩枪无冷却。 */
};

}  // namespace grappling_hook
