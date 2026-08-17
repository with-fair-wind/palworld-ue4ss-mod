/**
 * @file game_reflection.hpp
 * @brief 跨功能共用的 UE4SS 反射原语：UObject 有效性检查与无参反射调用。
 * @details 这些原语被背包/物品、技能、属性、形态等多个功能模块共用，从 pal_game.hpp
 *          提取以便复用。所有接口都只能在 Unreal 初始化完成后的游戏线程调用；返回的
 *          Unreal 裸指针均为非拥有观察指针，不会延长游戏对象生命周期。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
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

/** @brief 检查 UFunction 的全部 CPF_Parm 属性数量是否精确匹配。 */
[[nodiscard]] inline auto has_exact_parameter_count(RC::Unreal::UFunction* function,
                                                    const std::size_t expected) -> bool {
    if (function == nullptr) {
        return false;
    }
    std::size_t count{};
    for (auto* property : RC::Unreal::TFieldRange<RC::Unreal::FProperty>(
             function, RC::Unreal::EFieldIterationFlags::IncludeDeprecated)) {
        if (property->HasAnyPropertyFlags(RC::Unreal::CPF_Parm)) {
            ++count;
        }
    }
    return count == expected;
}

/**
 * @brief 判断属性是否是输入参数。
 * @details Unreal 可能为 `const T&` 同时设置 `CPF_ConstParm`、`CPF_ReferenceParm` 和
 *          `CPF_OutParm`。这类参数语义上仍是只读输入，不能因带有 OutParm 位而拒绝。
 */
[[nodiscard]] inline auto is_input_parameter(RC::Unreal::FProperty* property) -> bool {
    using namespace RC::Unreal;
    return property != nullptr && property->HasAnyPropertyFlags(CPF_Parm) &&
           !property->HasAnyPropertyFlags(CPF_ReturnParm) &&
           (!property->HasAnyPropertyFlags(CPF_OutParm) ||
            property->HasAnyPropertyFlags(CPF_ConstParm));
}

/** @brief 判断属性是否是非返回值输出参数。 */
[[nodiscard]] inline auto is_output_parameter(RC::Unreal::FProperty* property) -> bool {
    using namespace RC::Unreal;
    return property != nullptr && property->HasAnyPropertyFlags(CPF_Parm) &&
           property->HasAnyPropertyFlags(CPF_OutParm) &&
           !property->HasAnyPropertyFlags(CPF_ReturnParm);
}

/** @brief 判断属性是否是函数返回值。 */
[[nodiscard]] inline auto is_return_parameter(RC::Unreal::FProperty* property) -> bool {
    using namespace RC::Unreal;
    return property != nullptr && property->HasAnyPropertyFlags(CPF_Parm) &&
           property->HasAnyPropertyFlags(CPF_ReturnParm);
}

namespace detail {
/** @brief 检查整数 C++ 类型是否与具体 Unreal 数值属性类型精确对应。 */
template <typename T>
[[nodiscard]] inline auto matches_integral_property(RC::Unreal::FProperty* property) -> bool {
    using namespace RC::Unreal;
    using Value = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<Value, std::int8_t>) {
        return CastField<FInt8Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::uint8_t>) {
        return CastField<FByteProperty>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::int16_t>) {
        return CastField<FInt16Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::uint16_t>) {
        return CastField<FUInt16Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::int32_t>) {
        return CastField<FIntProperty>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::uint32_t>) {
        return CastField<FUInt32Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::int64_t>) {
        return CastField<FInt64Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
        return CastField<FUInt64Property>(property) != nullptr;
    } else {
        return false;
    }
}
}  // namespace detail

/**
 * @brief 从对象实际类链调用一个无输入参数、返回指定受支持类型的函数。
 * @tparam T 支持 UObject*、bool、有符号/无符号整数和 FName。
 * @return 签名精确匹配时返回调用结果；对象或元数据不可用时返回 std::nullopt。
 */
template <typename T>
[[nodiscard]] inline auto invoke(RC::Unreal::UObject* object, const TCHAR* functionName)
    -> std::optional<T> {
    using namespace RC::Unreal;
    if (!is_valid(object)) {
        return std::nullopt;
    }
    auto* const function = object->GetFunctionByNameInChain(functionName);
    auto* const returnProperty = function == nullptr ? nullptr : function->GetReturnProperty();
    if (!has_exact_parameter_count(function, 1) || !is_return_parameter(returnProperty)) {
        return std::nullopt;
    }

    if constexpr (std::is_same_v<T, UObject*>) {
        auto* const property = CastField<FObjectPropertyBase>(returnProperty);
        if (property == nullptr) {
            return std::nullopt;
        }
        FunctionParams params{function};
        object->ProcessEvent(function, params.data());
        auto* const result =
            property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(params.data()));
        return is_valid(result) ? std::optional<T>{result} : std::nullopt;
    } else if constexpr (std::is_same_v<T, bool>) {
        auto* const property = CastField<FBoolProperty>(returnProperty);
        if (property == nullptr) {
            return std::nullopt;
        }
        FunctionParams params{function};
        object->ProcessEvent(function, params.data());
        return property->GetPropertyValueInContainer(params.data());
    } else if constexpr (std::is_integral_v<T>) {
        auto* const property = CastField<FNumericProperty>(returnProperty);
        if (property == nullptr || !detail::matches_integral_property<T>(property)) {
            return std::nullopt;
        }
        FunctionParams params{function};
        object->ProcessEvent(function, params.data());
        auto* const value = property->ContainerPtrToValuePtr<void>(params.data());
        if constexpr (std::is_signed_v<T>) {
            return static_cast<T>(property->GetSignedIntPropertyValue(value));
        } else {
            return static_cast<T>(property->GetUnsignedIntPropertyValue(value));
        }
    } else if constexpr (std::is_same_v<T, FName>) {
        auto* const property = CastField<FNameProperty>(returnProperty);
        if (property == nullptr) {
            return std::nullopt;
        }
        FunctionParams params{function};
        object->ProcessEvent(function, params.data());
        return property->GetPropertyValueInContainer(params.data());
    } else {
        static_assert(!sizeof(T), "pal_game::invoke does not support this return type");
    }
}

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
