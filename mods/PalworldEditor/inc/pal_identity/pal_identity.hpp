/**
 * @file pal_identity.hpp
 * @brief 声明把 Alpha、Lucky 与觉醒领域请求适配到 Palworld 反射接口的网关。
 */
#pragma once

#include <pal_identity/pal_identity_editor.hpp>

namespace pal_identity {
/**
 * @brief 只在游戏线程使用的个体身份反射网关。
 * @details 不拥有、不缓存 UObject；每次调用都完整预检、写入、重读验证并在失败时回滚。
 */
class PalIdentityGateway final {
public:
    [[nodiscard]] auto read_identity(PalIdentityTarget target, bool spawnStateKnown,
                                     bool selectedIsSpawned) -> PalIdentitySnapshot;

    [[nodiscard]] auto apply_identity_edit(PalIdentityTarget target, bool spawnStateKnown,
                                           bool selectedIsSpawned,
                                           const PalIdentityEditRequest& request)
        -> PalIdentityEditResult;
};
}  // namespace pal_identity
