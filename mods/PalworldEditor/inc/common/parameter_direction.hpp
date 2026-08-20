/**
 * @file parameter_direction.hpp
 * @brief UFunction 参数方向（输入/输出/返回值）的纯标志判断，与 Unreal 类型解耦以便单测。
 * @details Unreal 反射参数同时携带 CPF_Parm/CPF_OutParm/CPF_ConstParm/CPF_ReturnParm 的
 *          组合位；`const T&` 可能同时带 CPF_ConstParm、CPF_ReferenceParm 与 CPF_OutParm，
 *          语义仍是只读输入。本头文件只接收布尔标志，由
 *          `common/game_reflection.hpp` 的 FProperty 包装函数映射调用。
 */
#pragma once

namespace pal_game {

/**
 * @brief 判断参数标志组合是否表示"输入参数"。
 * @param[in] parm       是否带 CPF_Parm。
 * @param[in] outParm    是否带 CPF_OutParm。
 * @param[in] constParm  是否带 CPF_ConstParm。
 * @param[in] returnParm 是否带 CPF_ReturnParm。
 * @retval true 该组合是只读输入（含 `const T&`）。
 */
[[nodiscard]] constexpr auto is_input_direction(const bool parm, const bool outParm,
                                                const bool constParm,
                                                const bool returnParm) noexcept -> bool {
    return parm && !returnParm && (!outParm || constParm);
}

/**
 * @brief 判断参数标志组合是否表示"非 const 非返回值输出参数"。
 * @details `const T&`（constParm+outParm）不是可写输出，必须排除。
 * @return 与 @ref is_input_direction 互斥：同一组合不可能同时是输入和可写输出。
 */
[[nodiscard]] constexpr auto is_output_direction(const bool parm, const bool outParm,
                                                 const bool constParm,
                                                 const bool returnParm) noexcept -> bool {
    return parm && !returnParm && outParm && !constParm;
}

/** @brief 判断参数标志组合是否表示函数返回值。 */
[[nodiscard]] constexpr auto is_return_direction(const bool parm, const bool /*outParm*/,
                                                 const bool /*constParm*/,
                                                 const bool returnParm) noexcept -> bool {
    return parm && returnParm;
}

}  // namespace pal_game
