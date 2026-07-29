#pragma once

// Studio-local demo components. AstraStudio is its own binary with a fresh
// TypeID budget; these are NOT the AstraTest components.

namespace Studio
{
    struct Position { float x = 0, y = 0, z = 0; };
    struct Velocity { float dx = 0, dy = 0, dz = 0; };
    struct Health
    {
        static constexpr bool AstraEnableable = true;   // demo data for the Memory panel's disabled-bit layers
        int current = 100, max = 100;
    };
    struct Sprite   { int textureId = 0; float scale = 1.0f; };
    struct Frozen   { };   // tag
}
