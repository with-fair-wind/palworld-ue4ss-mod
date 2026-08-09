/**
 * @file game_reflection.hpp
 * @brief 跨功能共用的 UE4SS 反射原语：UObject 有效性检查与无参反射调用。
 * @details 这些原语被背包/物品、技能、属性、形态等多个功能模块共用，从 pal_game.hpp
 *          提取以便复用。所有接口都只能在 Unreal 初始化完成后的游戏线程调用；返回的
 *          Unreal 裸指针均为非拥有观察指针，不会延长游戏对象生命周期。
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

namespace pal_game {
/** @brief 主玩家背包数据对象的 Unreal 类名。 */
inline constexpr const TCHAR* kInventoryClassName = STR("PalPlayerInventoryData");

/**
 * @brief 对 UObject 观察指针执行轻量有效性检查。
 * @param[in] obj 待检查的非拥有 UObject 指针。
 * @retval true 指针非空且仍能取得类元数据。
 * @retval false 指针为空或对象的类元数据已经失效。
 * @warning 本检查不能延长对象生命周期，也不能保证对象在后续帧仍然有效。
 */
inline auto is_valid(RC::Unreal::UObject* obj) -> bool {
    return obj != nullptr && obj->GetClassPrivate() != nullptr;
}

/**
 * @brief 从对象实际类链调用一个无参数、返回指定类型的函数。
 * @tparam T 返回值 C++ 类型（int/bool/FName/UObject* 等），须与 UFunction 的 ReturnValue
 *           字段布局匹配。
 * @param[in] object 非拥有调用目标。
 * @param[in] functionName 要从实际类开始查找的函数名。
 * @return 函数返回值；目标或函数不可用时返回 std::nullopt。指针返回值额外经 is_valid 校验。
 */
template <typename T>
[[nodiscard]] inline auto invoke(RC::Unreal::UObject* object, const TCHAR* functionName)
    -> std::optional<T> {
    if (!is_valid(object)) {
        return std::nullopt;
    }
    auto* const function = object->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return std::nullopt;
    }
    struct Params {
        T ReturnValue{};
    } params;
    object->ProcessEvent(function, &params);
    if constexpr (std::is_pointer_v<T>) {
        return is_valid(params.ReturnValue) ? std::optional<T>{params.ReturnValue} : std::nullopt;
    } else {
        return params.ReturnValue;
    }
}

/**
 * @brief RAII 包装 UFunction 参数缓冲区（构造时 InitializeStruct、析构时 DestroyStruct）。
 * @details function 为 null 时为空操作；data() 返回的内存仅在本对象存活期间有效。
 */
class FunctionParams final {
public:
    explicit FunctionParams(RC::Unreal::UFunction* function)
        : function_{function},
          storage_(function == nullptr ? 0U : static_cast<std::size_t>(function->GetParmsSize())) {
        if (function_ != nullptr) {
            function_->InitializeStruct(storage_.data());
        }
    }

    FunctionParams(const FunctionParams&) = delete;
    auto operator=(const FunctionParams&) -> FunctionParams& = delete;
    FunctionParams(FunctionParams&&) = delete;
    auto operator=(FunctionParams&&) -> FunctionParams& = delete;

    ~FunctionParams() {
        if (function_ != nullptr) {
            function_->DestroyStruct(storage_.data());
        }
    }

    [[nodiscard]] auto data() noexcept -> void* {
        return storage_.data();
    }

private:
    RC::Unreal::UFunction* function_{};
    std::vector<std::byte> storage_;
};

/** @brief 从完整对象名恢复 UObject；类名前缀存在时只传递后半对象路径。 */
[[nodiscard]] inline auto find_object_by_full_name(const std::wstring& fullName)
    -> RC::Unreal::UObject* {
    if (fullName.empty()) {
        return nullptr;
    }
    const auto separator = fullName.find(L' ');
    const auto objectPath =
        separator == std::wstring::npos ? fullName : fullName.substr(separator + 1);
    return RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(nullptr, nullptr,
                                                                              objectPath.c_str());
}
}  // namespace pal_game
