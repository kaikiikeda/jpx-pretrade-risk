// A deliberately tiny test harness, so the project builds with no dependencies.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

inline int run_all() {
    int failed_cases = 0;
    for (auto& c : registry()) {
        const int before = failures();
        c.fn();
        const bool ok = failures() == before;
        if (!ok) ++failed_cases;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
    }
    std::printf("\n%zu test cases, %d failed, %d failed checks\n", registry().size(), failed_cases, failures());
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace t

#define T_CAT2(a, b) a##b
#define T_CAT(a, b) T_CAT2(a, b)
#define TEST(name) \
    static void T_CAT(test_, __LINE__)(); \
    static t::Reg T_CAT(reg_, __LINE__)(name, T_CAT(test_, __LINE__)); \
    static void T_CAT(test_, __LINE__)()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++t::failures();                                                          \
            std::printf("    %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const auto _a = (a);                                                                        \
        const auto _b = (b);                                                                        \
        if (!(_a == _b)) {                                                                          \
            ++t::failures();                                                                        \
            std::printf("    %s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b);      \
        }                                                                                           \
    } while (0)
