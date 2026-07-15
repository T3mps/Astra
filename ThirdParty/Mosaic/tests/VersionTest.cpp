// Scaffolding smoke: proves the library links and runs. Replaced/joined by the
// real Platform / FunctionRef / IWorkScheduler suites as the extraction lands.

#include <catch2/catch_test_macros.hpp>

#include <Mosaic/Version.hpp>

#include <string_view>

TEST_CASE("Mosaic version is reported", "[mosaic]")
{
    CHECK(std::string_view(Mosaic::Version()) == "0.1.0");
    CHECK(Mosaic::kVersionMajor == 0);
    CHECK(Mosaic::kVersionMinor == 1);
    CHECK(Mosaic::kVersionPatch == 0);
}
