// MosaicTests runner entry point. The vendored Catch2 wrapper excludes
// catch_main.cpp, so the test executable provides its own main(). Mosaic is
// dependency-free -- there is no engine/ECS context to install.

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    return Catch::Session().run(argc, argv);
}
