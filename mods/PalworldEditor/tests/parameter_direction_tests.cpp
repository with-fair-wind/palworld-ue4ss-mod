/**
 * @file parameter_direction_tests.cpp
 * @brief 参数方向纯标志判定测试：覆盖 CPF_Parm/OutParm/ConstParm/ReturnParm 全部 16 种组合。
 */
#include <iostream>

#include <common/parameter_direction.hpp>
#include <common/player_state_gate.hpp>
namespace {
int failures = 0;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
}  // namespace

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_all_flag_combinations() {
    for (int flags = 0; flags < 16; ++flags) {
        const bool parm = (flags & 1) != 0;
        const bool outParm = (flags & 2) != 0;
        const bool constParm = (flags & 4) != 0;
        const bool returnParm = (flags & 8) != 0;
        const auto expectedInput = parm && !returnParm && (!outParm || constParm);
        const auto expectedOutput = parm && !returnParm && outParm && !constParm;
        const auto expectedReturn = parm && returnParm;
        if (pal_game::is_input_direction(parm, outParm, constParm, returnParm) != expectedInput) {
            std::cerr << "FAIL flags=" << flags << ": input mismatch\n";
            ++failures;
        }
        if (pal_game::is_output_direction(parm, outParm, constParm, returnParm) != expectedOutput) {
            std::cerr << "FAIL flags=" << flags << ": output mismatch\n";
            ++failures;
        }
        if (pal_game::is_return_direction(parm, outParm, constParm, returnParm) != expectedReturn) {
            std::cerr << "FAIL flags=" << flags << ": return mismatch\n";
            ++failures;
        }
        // 输入与可写输出必须互斥；const T&（parm+out+const）只能判为输入。
        if (expectedInput && expectedOutput) {
            std::cerr << "FAIL flags=" << flags << ": input and output both true\n";
            ++failures;
        }
    }
}

void test_const_reference_is_input_only() {
    // const T&：CPF_Parm | CPF_OutParm | CPF_ConstParm，无 ReturnParm。
    CHECK(pal_game::is_input_direction(true, true, true, false));
    CHECK(!pal_game::is_output_direction(true, true, true, false));
    CHECK(!pal_game::is_return_direction(true, true, true, false));
}

void test_plain_output_is_writable() {
    // T& 输出：CPF_Parm | CPF_OutParm，无 ConstParm。
    CHECK(!pal_game::is_input_direction(true, true, false, false));
    CHECK(pal_game::is_output_direction(true, true, false, false));
    CHECK(!pal_game::is_return_direction(true, true, false, false));
}

void test_return_value_is_never_input_or_output() {
    CHECK(!pal_game::is_input_direction(true, false, false, true));
    CHECK(!pal_game::is_output_direction(true, false, false, true));
    CHECK(pal_game::is_return_direction(true, false, false, true));
}

void test_player_state_gate_is_fail_closed() {
    CHECK(!pal_game::state_gate_allows(std::nullopt));
    CHECK(!pal_game::state_gate_allows(std::optional<bool>{true}));
    CHECK(pal_game::state_gate_allows(std::optional<bool>{false}));
}

int main() {
    test_all_flag_combinations();
    test_const_reference_is_input_only();
    test_plain_output_is_writable();
    test_return_value_is_never_input_or_output();
    test_player_state_gate_is_fail_closed();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
