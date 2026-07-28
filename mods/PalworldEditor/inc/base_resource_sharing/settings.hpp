#pragma once

/** @brief 提供跨据点资源共享自身的独立配置值。 */
namespace base_resource_sharing {

/** @brief `[BaseResourceSharing]` 配置节；默认失败安全地关闭。 */
struct Settings {
    bool enabled{}; /**< 是否期望启用同公会跨据点材料共享。 */
};
}  // namespace base_resource_sharing
