#include <catch2/catch_test_macros.hpp>

#include <Mosaic/FunctionRef.hpp>

namespace
{
    int Triple(int x) { return x * 3; }
}

TEST_CASE("Mosaic::FunctionRef views a capturing lambda", "[mosaic][functionref]")
{
    int captured = 10;
    auto add = [&](int x) { captured += x; return captured; };
    Mosaic::FunctionRef<int(int)> ref = add;

    REQUIRE(static_cast<bool>(ref));
    CHECK(ref(5) == 15);
    CHECK(ref(5) == 20);
    CHECK(captured == 20);
}

TEST_CASE("Mosaic::FunctionRef binds a free function and forwards args", "[mosaic][functionref]")
{
    Mosaic::FunctionRef<int(int)> ref = Triple;
    CHECK(ref(7) == 21);

    auto sum3 = [](int a, int b, int c) { return a + b + c; };
    Mosaic::FunctionRef<int(int, int, int)> ref3 = sum3;
    CHECK(ref3(1, 2, 3) == 6);
}

TEST_CASE("Mosaic::FunctionRef default-constructs empty", "[mosaic][functionref]")
{
    Mosaic::FunctionRef<void()> ref;
    CHECK_FALSE(static_cast<bool>(ref));
}
