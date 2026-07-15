#pragma once

// Mosaic library version. The first real module (Platform) lands next; this
// exists so the freshly-scaffolded library has a compiled unit and a smoke test.

namespace Mosaic
{
    inline constexpr int kVersionMajor = 0;
    inline constexpr int kVersionMinor = 1;
    inline constexpr int kVersionPatch = 0;

    // "major.minor.patch" -- defined in src/Version.cpp.
    const char* Version() noexcept;
}
