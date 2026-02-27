#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../Component/Component.hpp"
#include "../Container/SmallVector.hpp"
#include "../Entity/Entity.hpp"
#include "Base.hpp"
#include "Delegate.hpp"

namespace Astra
{
    enum class Signal : uint32_t
    {
        None             = 0,
        EntityCreated    = 1 << 0,
        EntityDestroyed  = 1 << 1,
        ComponentAdded   = 1 << 2,
        ComponentRemoved = 1 << 3,
        ComponentUpdated = 1 << 4,
        ParentChanged    = 1 << 5,
        LinkAdded        = 1 << 6,
        LinkRemoved      = 1 << 7,
        ResourceAdded    = 1 << 8,
        ResourceRemoved  = 1 << 9,
        ResourceUpdated  = 1 << 10,
        // Reserve space for future signals
        All = ~0u
    };
    
    inline Signal operator|(Signal a, Signal b) noexcept
    {
        return static_cast<Signal>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    
    inline Signal operator&(Signal a, Signal b) noexcept
    {
        return static_cast<Signal>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    
    inline Signal operator^(Signal a, Signal b) noexcept
    {
        return static_cast<Signal>(static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b));
    }
    
    inline Signal operator~(Signal a) noexcept
    {
        return static_cast<Signal>(~static_cast<uint32_t>(a));
    }
    
    inline Signal& operator|=(Signal& a, Signal b) noexcept
    {
        return a = a | b;
    }
    
    inline Signal& operator&=(Signal& a, Signal b) noexcept
    {
        return a = a & b;
    }
    
    inline Signal& operator^=(Signal& a, Signal b) noexcept
    {
        return a = a ^ b;
    }
    
    inline bool HasSignal(Signal flags, Signal signal) noexcept
    {
        return (flags & signal) != Signal::None;
    }

    template<typename T>
    concept Event = requires
    {
        { T::flag } -> std::convertible_to<Signal>;
    } && std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>;
    
    namespace Events
    {
        struct EntityCreated
        {
            static constexpr Signal flag = Signal::EntityCreated;
            Entity entity;
        };
        
        struct EntityDestroyed
        {
            static constexpr Signal flag = Signal::EntityDestroyed;
            Entity entity;
        };
        
        struct ComponentAdded
        {
            static constexpr Signal flag = Signal::ComponentAdded;
            Entity entity;
            ComponentID componentId;
            void* component;
        };
        
        struct ComponentRemoved
        {
            static constexpr Signal flag = Signal::ComponentRemoved;
            Entity entity;
            ComponentID componentId;
            void* component;
        };
        
        struct ComponentUpdated
        {
            static constexpr Signal flag = Signal::ComponentUpdated;
            Entity entity;
            ComponentID componentId;
            void* component;
        };
        
        struct ParentChanged
        {
            static constexpr Signal flag = Signal::ParentChanged;
            Entity child;
            Entity parent;
        };
        
        struct LinkAdded
        {
            static constexpr Signal flag = Signal::LinkAdded;
            Entity first;
            Entity second;
        };
        
        struct LinkRemoved
        {
            static constexpr Signal flag = Signal::LinkRemoved;
            Entity first;
            Entity second;
        };
        
        struct ResourceAdded
        {
            static constexpr Signal flag = Signal::ResourceAdded;
            ComponentID resourceId;
            void* resource;
        };
        
        struct ResourceRemoved
        {
            static constexpr Signal flag = Signal::ResourceRemoved;
            ComponentID resourceId;
            void* resource;
        };
        
        struct ResourceUpdated
        {
            static constexpr Signal flag = Signal::ResourceUpdated;
            ComponentID resourceId;
            void* resource;
        };
    }
    
    namespace Detail
    {
        template<typename Event, typename... Events>
        struct EventIndexImpl;
        
        template<typename Event>
        struct EventIndexImpl<Event>
        {
            static constexpr size_t value = size_t(-1);
        };
        
        template<typename Event, typename... Rest>
        struct EventIndexImpl<Event, Event, Rest...>
        {
            static constexpr size_t value = 0;
        };
        
        template<typename Event, typename First, typename... Rest>
        struct EventIndexImpl<Event, First, Rest...>
        {
            static constexpr size_t value = 1 + EventIndexImpl<Event, Rest...>::value;
        };
        
        template<typename Event, typename... Events>
        inline constexpr size_t EventIndex = EventIndexImpl<Event, Events...>::value;
    }
    
    class SignalManager
    {
    public:
        SignalManager() noexcept : m_enabledSignals(Signal::None) {}
        
        template<Event E, typename... Args>
        void Emit(Args&&... args)
        {
            if (IsEnabled<E>())
            {
                E event{std::forward<Args>(args)...};
                auto& handler = std::get<IndexOf<E>()>(m_handlers);
                handler(event);
            }
        }
        
        template<Event E>
        auto& On() noexcept
        {
            return std::get<IndexOf<E>()>(m_handlers);
        }
        
        template<Event E>
        const auto& On() const noexcept
        {
            return std::get<IndexOf<E>()>(m_handlers);
        }
        
        template<Event E>
        void Enable() noexcept
        {
            m_enabledSignals |= E::flag;
        }
        
        template<Event E>
        void Disable() noexcept
        {
            m_enabledSignals &= ~E::flag;
        }
        
        template<Event E>
        bool IsEnabled() const noexcept
        {
            return HasSignal(m_enabledSignals, E::flag);
        }
        
        void EnableSignals(Signal signals) noexcept
        {
            m_enabledSignals |= signals;
        }
        
        void DisableSignals(Signal signals) noexcept
        {
            m_enabledSignals &= ~signals;
        }
        
        void SetEnabledSignals(Signal signals) noexcept
        {
            m_enabledSignals = signals;
        }
        
        ASTRA_NODISCARD Signal GetEnabledSignals() const noexcept
        {
            return m_enabledSignals;
        }
        
        ASTRA_NODISCARD bool IsSignalEnabled(Signal signal) const noexcept
        {
            return HasSignal(m_enabledSignals, signal);
        }
        
        void ClearAllHandlers()
        {
            std::apply([](auto&... handler)
            {
                ((handler.Clear()), ...);
            }, m_handlers);
        }

    private:
        template<Event E>
        static constexpr size_t IndexOf() noexcept
        {
            return Detail::EventIndex<E,
                Events::EntityCreated,
                Events::EntityDestroyed,
                Events::ComponentAdded,
                Events::ComponentRemoved,
                Events::ComponentUpdated,
                Events::ParentChanged,
                Events::LinkAdded,
                Events::LinkRemoved,
                Events::ResourceAdded,
                Events::ResourceRemoved,
                Events::ResourceUpdated
            >;
        }

        std::tuple<
            MulticastDelegate<void(const Events::EntityCreated&)>,
            MulticastDelegate<void(const Events::EntityDestroyed&)>,
            MulticastDelegate<void(const Events::ComponentAdded&)>,
            MulticastDelegate<void(const Events::ComponentRemoved&)>,
            MulticastDelegate<void(const Events::ComponentUpdated&)>,
            MulticastDelegate<void(const Events::ParentChanged&)>,
            MulticastDelegate<void(const Events::LinkAdded&)>,
            MulticastDelegate<void(const Events::LinkRemoved&)>,
            MulticastDelegate<void(const Events::ResourceAdded&)>,
            MulticastDelegate<void(const Events::ResourceRemoved&)>,
            MulticastDelegate<void(const Events::ResourceUpdated&)>
        > m_handlers;

        Signal m_enabledSignals;
    };
}