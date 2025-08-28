#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Container/SmallVector.hpp"
#include "Base.hpp"

namespace Astra
{
    template<typename Signature>
    class Delegate;
    
    template<typename Signature>
    class MulticastDelegate;
    
    template<typename R, typename... Args>
    class Delegate<R(Args...)>
    {
    public:
        using ResultType = R;
        
        static constexpr size_t SMALL_BUFFER_SIZE = 32;
        
        Delegate() noexcept : m_invoker(nullptr) {}
        
        Delegate(std::nullptr_t) noexcept : m_invoker(nullptr) {}
        
        template<typename Func>
        Delegate(Func* func) noexcept : m_invoker(nullptr)
        {
            if (func)
            {
                using FuncType = Func*;
                static_assert(sizeof(FuncType) <= SMALL_BUFFER_SIZE, "Function pointer too large");
                
                new (m_storage) FuncType(func);
                m_invoker = &InvokeFunctionPointer<Func>;
            }
        }
        
        template<typename Func, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Func>, Delegate>>>
        Delegate(Func&& func) : m_invoker(nullptr)
        {
            using DecayedFunc = std::decay_t<Func>;
            
            if constexpr (sizeof(DecayedFunc) <= SMALL_BUFFER_SIZE && std::is_nothrow_move_constructible_v<DecayedFunc>)
            {
                new (m_storage) DecayedFunc(std::forward<Func>(func));
                m_invoker = &InvokeSmallFunctor<DecayedFunc>;
                m_manager = &ManageSmallFunctor<DecayedFunc>;
            }
            else
            {
                // Use shared_ptr for large functors to enable safe copying
                auto* heapFunc = new DecayedFunc(std::forward<Func>(func));
                *reinterpret_cast<std::shared_ptr<DecayedFunc>*>(m_storage) = std::shared_ptr<DecayedFunc>(heapFunc);
                m_invoker = &InvokeLargeFunctor<DecayedFunc>;
                m_manager = &ManageLargeFunctor<DecayedFunc>;
            }
        }
        
        template<typename T, typename MemberFunc>
        static Delegate FromMember(T* instance, MemberFunc T::*memberFunc)
        {
            Delegate delegate;
            
            struct MemberBinding
            {
                T* instance;
                MemberFunc T::*memberFunc;
            };
            
            static_assert(sizeof(MemberBinding) <= SMALL_BUFFER_SIZE, "Member binding too large");
            
            new (delegate.m_storage) MemberBinding{instance, memberFunc};
            delegate.m_invoker = &InvokeMemberFunction<T, MemberFunc>;
            delegate.m_manager = &ManageSmallFunctor<MemberBinding>;
            
            return delegate;
        }
        
        Delegate(const Delegate& other) : m_invoker(other.m_invoker), m_manager(other.m_manager)
        {
            if (m_invoker && m_manager)
            {
                m_manager(ManagerOp::Copy, m_storage, other.m_storage);
            }
            else if (m_invoker)
            {
                // Function pointer - simple copy
                std::memcpy(m_storage, other.m_storage, sizeof(void*));
            }
        }
        
        Delegate(Delegate&& other) noexcept : m_invoker(other.m_invoker), m_manager(other.m_manager)
        {
            if (m_invoker && m_manager)
            {
                m_manager(ManagerOp::Move, m_storage, other.m_storage);
            }
            else if (m_invoker)
            {
                // Function pointer - simple copy
                std::memcpy(m_storage, other.m_storage, sizeof(void*));
            }
            other.m_invoker = nullptr;
            other.m_manager = nullptr;
        }
        
        ~Delegate()
        {
            Reset();
        }
        
        Delegate& operator=(const Delegate& other)
        {
            if (this != &other)
            {
                Reset();
                m_invoker = other.m_invoker;
                m_manager = other.m_manager;
                if (m_invoker && m_manager)
                {
                    m_manager(ManagerOp::Copy, m_storage, other.m_storage);
                }
                else if (m_invoker)
                {
                    // Function pointer - simple copy
                    std::memcpy(m_storage, other.m_storage, sizeof(void*));
                }
            }
            return *this;
        }
        
        Delegate& operator=(Delegate&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                m_invoker = other.m_invoker;
                m_manager = other.m_manager;
                if (m_invoker && m_manager)
                {
                    m_manager(ManagerOp::Move, m_storage, other.m_storage);
                }
                else if (m_invoker)
                {
                    // Function pointer - simple copy
                    std::memcpy(m_storage, other.m_storage, sizeof(void*));
                }
                other.m_invoker = nullptr;
                other.m_manager = nullptr;
            }
            return *this;
        }
        
        Delegate& operator=(std::nullptr_t) noexcept
        {
            Reset();
            return *this;
        }
        
        R operator()(Args... args) const
        {
            ASTRA_ASSERT(m_invoker != nullptr, "Calling empty delegate");
            return m_invoker(m_storage, std::forward<Args>(args)...);
        }
        
        explicit operator bool() const noexcept
        {
            return m_invoker != nullptr;
        }
        
        void Reset() noexcept
        {
            if (m_manager)
            {
                m_manager(ManagerOp::Destroy, m_storage, nullptr);
            }
            m_invoker = nullptr;
            m_manager = nullptr;
        }
        
        bool operator==(const Delegate& other) const noexcept
        {
            if (m_invoker != other.m_invoker)
                return false;
                
            if (m_invoker == nullptr)
                return true;
                
            if (m_manager == nullptr && other.m_manager == nullptr)
            {
                return std::memcmp(m_storage, other.m_storage, sizeof(void*)) == 0;
            }
            
            return false;
        }
        
        bool operator!=(const Delegate& other) const noexcept
        {
            return !(*this == other);
        }
        
    private:
        using InvokerType = R(*)(const void*, Args...);
        
        enum class ManagerOp
        {
            Copy,
            Move,
            Destroy
        };
        
        using ManagerType = void(*)(ManagerOp, void*, const void*);
        
        template<typename Func>
        static R InvokeFunctionPointer(const void* storage, Args... args)
        {
            auto func = *reinterpret_cast<Func* const*>(storage);
            return func(std::forward<Args>(args)...);
        }
        
        template<typename Func>
        static R InvokeSmallFunctor(const void* storage, Args... args)
        {
            auto& func = *const_cast<Func*>(reinterpret_cast<const Func*>(storage));
            return func(std::forward<Args>(args)...);
        }
        
        template<typename Func>
        static R InvokeLargeFunctor(const void* storage, Args... args)
        {
            auto& sharedPtr = *const_cast<std::shared_ptr<Func>*>(reinterpret_cast<const std::shared_ptr<Func>*>(storage));
            return (*sharedPtr)(std::forward<Args>(args)...);
        }
        
        template<typename T, typename MemberFunc>
        static R InvokeMemberFunction(const void* storage, Args... args)
        {
            struct MemberBinding
            {
                T* instance;
                MemberFunc T::*memberFunc;
            };
            
            const auto& binding = *reinterpret_cast<const MemberBinding*>(storage);
            return (binding.instance->*binding.memberFunc)(std::forward<Args>(args)...);
        }
        
        template<typename Func>
        static void ManageSmallFunctor(ManagerOp op, void* dst, const void* src)
        {
            switch (op)
            {
                case ManagerOp::Copy:
                    if constexpr (std::is_copy_constructible_v<Func>)
                    {
                        new (dst) Func(*reinterpret_cast<const Func*>(src));
                    }
                    else
                    {
                        ASTRA_ASSERT(false, "Functor is not copy constructible");
                    }
                    break;
                case ManagerOp::Move:
                    new (dst) Func(std::move(*reinterpret_cast<Func*>(const_cast<void*>(src))));
                    reinterpret_cast<Func*>(const_cast<void*>(src))->~Func();
                    break;
                case ManagerOp::Destroy:
                    reinterpret_cast<Func*>(dst)->~Func();
                    break;
            }
        }
        
        template<typename Func>
        static void ManageLargeFunctor(ManagerOp op, void* dst, const void* src)
        {
            using SharedType = std::shared_ptr<Func>;
            
            switch (op)
            {
                case ManagerOp::Copy:
                    new (dst) SharedType(*reinterpret_cast<const SharedType*>(src));
                    break;
                case ManagerOp::Move:
                    new (dst) SharedType(std::move(*reinterpret_cast<SharedType*>(const_cast<void*>(src))));
                    reinterpret_cast<SharedType*>(const_cast<void*>(src))->~SharedType();
                    break;
                case ManagerOp::Destroy:
                    reinterpret_cast<SharedType*>(dst)->~SharedType();
                    break;
            }
        }

        alignas(std::max_align_t) mutable std::byte m_storage[SMALL_BUFFER_SIZE];
        InvokerType m_invoker;
        ManagerType m_manager = nullptr;
    };
    
    template<typename R, typename... Args>
    class MulticastDelegate<R(Args...)>
    {
    public:
        using DelegateType = Delegate<R(Args...)>;
        using ResultType = R;
        using HandlerID = std::size_t;
        
        MulticastDelegate() = default;
        
        struct Handler
        {
            HandlerID id;
            DelegateType delegate;
        };
        
        HandlerID Register(const DelegateType& delegate)
        {
            if (delegate)
            {
                HandlerID id = m_nextID++;
                m_handlers.push_back({id, delegate});
                return id;
            }
            return 0;
        }
        
        HandlerID Register(DelegateType&& delegate)
        {
            if (delegate)
            {
                HandlerID id = m_nextID++;
                m_handlers.push_back({id, std::move(delegate)});
                return id;
            }
            return 0;
        }
        
        template<typename Func>
        HandlerID Register(Func&& func)
        {
            HandlerID id = m_nextID++;
            m_handlers.push_back({id, DelegateType(std::forward<Func>(func))});
            return id;
        }
        
        template<typename T, typename MemberFunc>
        HandlerID RegisterMember(T* instance, MemberFunc T::*memberFunc)
        {
            HandlerID id = m_nextID++;
            m_handlers.push_back({id, DelegateType::FromMember(instance, memberFunc)});
            return id;
        }
        
        bool Unregister(HandlerID id)
        {
            auto it = std::find_if(m_handlers.begin(), m_handlers.end(),
                                  [id](const Handler& h) { return h.id == id; });
            if (it != m_handlers.end())
            {
                m_handlers.erase(it);
                return true;
            }
            return false;
        }
        
        void Clear()
        {
            m_handlers.clear();
        }
        
        size_t Size() const noexcept
        {
            return m_handlers.size();
        }
        
        bool IsEmpty() const noexcept
        {
            return m_handlers.empty();
        }
        
        template<typename U = R>
        std::enable_if_t<std::is_void_v<U>> Invoke(Args... args) const
        {
            for (const auto& handler : m_handlers)
            {
                handler.delegate(std::forward<Args>(args)...);
            }
        }
        
        template<typename U = R>
        std::enable_if_t<!std::is_void_v<U>, SmallVector<R, 4>> Invoke(Args... args) const
        {
            SmallVector<R, 4> results;
            results.reserve(m_handlers.size());
            
            for (const auto& handler : m_handlers)
            {
                results.push_back(handler.delegate(std::forward<Args>(args)...));
            }
            
            return results;
        }
        
        template<typename U = R>
        std::enable_if_t<std::is_void_v<U>> operator()(Args... args) const
        {
            Invoke(std::forward<Args>(args)...);
        }
        
        template<typename U = R>
        std::enable_if_t<!std::is_void_v<U>, SmallVector<R, 4>> operator()(Args... args) const
        {
            return Invoke(std::forward<Args>(args)...);
        }
        
    private:
        std::vector<Handler> m_handlers;
        HandlerID m_nextID = 1;
    };
}