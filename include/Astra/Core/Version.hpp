#pragma once

#define ASTRA_VERSION_MAJOR 2
#define ASTRA_VERSION_MINOR 1
#define ASTRA_VERSION_PATCH 0

#define ASTRA_VERSION ((ASTRA_VERSION_MAJOR << 16) | (ASTRA_VERSION_MINOR << 8) | ASTRA_VERSION_PATCH)

namespace Astra
{
    inline constexpr int VERSION_MAJOR = ASTRA_VERSION_MAJOR;
    inline constexpr int VERSION_MINOR = ASTRA_VERSION_MINOR;
    inline constexpr int VERSION_PATCH = ASTRA_VERSION_PATCH;
    inline constexpr int VERSION = ASTRA_VERSION;
}