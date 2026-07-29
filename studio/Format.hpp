#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

namespace Studio
{
    // Shared byte formatter for panel labels (was duplicated per-panel).
    inline std::string FormatBytes(size_t b)
    {
        char buf[32];
        if (b >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.2f MB", double(b) / (1024.0 * 1024.0));
        else if (b >= 1024)   std::snprintf(buf, sizeof(buf), "%.1f KB", double(b) / 1024.0);
        else                  std::snprintf(buf, sizeof(buf), "%zu B", b);
        return buf;
    }
}
