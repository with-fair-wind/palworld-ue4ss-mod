#include <iostream>

namespace
{
auto failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition)
    {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
}

#define CHECK(expression) check((expression), #expression, __LINE__)

auto main() -> int
{
    CHECK(true);
    return failures == 0 ? 0 : 1;
}
