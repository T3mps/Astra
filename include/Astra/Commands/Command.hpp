#pragma once

#include <span>
#include "../Core/Delegate.hpp"

namespace Astra
{
    class Registry;
    
    struct Command
    {
        Delegate<bool(Registry*)> execute;  // Returns true on success, false on failure
        
        enum class Type : uint8_t
        {
            Invalid,
            Entity,         // Entity creation/destruction
            Component,      // Component add/remove/modify
            Relationship,   // Parent/child/link operations
            Resource        // Global resource operations
        };
        
        Type type = Type::Invalid;
        
#ifdef ASTRA_BUILD_DEBUG
        const char* debugName = nullptr;
#endif
        
        Command(Delegate<bool(Registry*)> exec, Type cmdType = Type::Invalid, ASTRA_MAYBE_UNUSED const char* name = nullptr) :
            execute(std::move(exec)),
            type(cmdType)
        #ifdef ASTRA_BUILD_DEBUG
            , debugName(name)
        #endif
        {}
    };
} // namespace Astra
