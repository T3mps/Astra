#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "../Core/Base.hpp"
#include "../Core/TypeID.hpp"

namespace Astra
{
    // Type-erased, RTTI-free single-value box tagged by Astra TypeID::Hash().
    // Replaces the standard library's type-erased any container on the reflection
    // dynamic path so reflection builds and runs with RTTI disabled -- deterministically,
    // and correctly across DSO/DLL boundaries (a stable name-hash tag, unlike that
    // container's manager-pointer / runtime-type identity that -fno-rtti compiles out).
    // Value semantics; a wrong-T TryCast returns nullptr (never UB, never throws). SBO
    // keeps common reflected field types (int/float/Vec3/Vec4/pointer) allocation-free;
    // larger/over-aligned types spill to an aligned heap block. The manager is a per-type
    // function-pointer vtable (destroy/copy/move) -- no runtime type queries of any kind.
    // Astra is exception-free, so constructors are assumed non-throwing (a throwing ctor /
    // bad_alloc terminates, as everywhere in Astra). The converting constructor is
    // deliberately explicit (unlike std::any's implicit one) -- all call sites construct
    // explicitly, so this only rules out silent conversions.
    class AnyValue
    {
    public:
        static constexpr std::size_t kInlineSize  = 16;                       // two doubles (EnTT/Folly default)
        static constexpr std::size_t kInlineAlign = alignof(std::max_align_t);

        AnyValue() noexcept = default;

        template<typename T, typename D = std::decay_t<T>,
                 typename = std::enable_if_t<!std::is_same_v<D, AnyValue>>>
        explicit AnyValue(T&& value)
        {
            EmplaceImpl<D>(std::forward<T>(value));
        }

        AnyValue(const AnyValue& other) { CopyFrom(other); }
        AnyValue(AnyValue&& other) noexcept { MoveFrom(other); }

        AnyValue& operator=(const AnyValue& other)
        {
            if (this != &other) { Reset(); CopyFrom(other); }
            return *this;
        }
        AnyValue& operator=(AnyValue&& other) noexcept
        {
            if (this != &other) { Reset(); MoveFrom(other); }
            return *this;
        }
        ~AnyValue() { Reset(); }

        ASTRA_NODISCARD bool HasValue() const noexcept { return m_vtable != nullptr; }
        ASTRA_NODISCARD uint64_t TypeHash() const noexcept { return m_typeHash; }

        template<typename T, typename D = std::decay_t<T>>
        ASTRA_NODISCARD const D* TryCast() const noexcept
        {
            if (!m_vtable || m_typeHash != TypeID<D>::Hash()) return nullptr;
            return static_cast<const D*>(Data());
        }
        template<typename T, typename D = std::decay_t<T>>
        ASTRA_NODISCARD D* TryCast() noexcept
        {
            if (!m_vtable || m_typeHash != TypeID<D>::Hash()) return nullptr;
            return static_cast<D*>(Data());
        }

    private:
        struct VTable
        {
            void (*destroy)(void* obj);
            void (*copyConstruct)(void* dst, const void* src);
            void (*moveConstruct)(void* dst, void* src);
            std::size_t size;
            std::size_t align;
        };

        template<typename T>
        static const VTable* VTableFor() noexcept
        {
            static const VTable vt{
                [](void* obj) { static_cast<T*>(obj)->~T(); },
                [](void* dst, const void* src) { ::new (dst) T(*static_cast<const T*>(src)); },
                [](void* dst, void* src) { ::new (dst) T(std::move(*static_cast<T*>(src))); },
                sizeof(T),
                alignof(T)
            };
            return &vt;
        }

        template<typename T>
        static constexpr bool FitsInline() noexcept
        {
            return sizeof(T) <= kInlineSize
                && alignof(T) <= kInlineAlign
                && std::is_nothrow_move_constructible_v<T>;
        }

        static void* Allocate(std::size_t size, std::size_t align)
        {
            if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
                return ::operator new(size, std::align_val_t{align});
            return ::operator new(size);
        }
        static void Deallocate(void* ptr, std::size_t align) noexcept
        {
            if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
                ::operator delete(ptr, std::align_val_t{align});
            else
                ::operator delete(ptr);
        }

        void* Data() noexcept { return m_heap ? m_heap : static_cast<void*>(m_inline); }
        const void* Data() const noexcept { return m_heap ? m_heap : static_cast<const void*>(m_inline); }

        template<typename T, typename Arg>
        void EmplaceImpl(Arg&& arg)
        {
            void* storage;
            if constexpr (FitsInline<T>())
            {
                m_heap = nullptr;
                storage = m_inline;
            }
            else
            {
                m_heap = Allocate(sizeof(T), alignof(T));
                storage = m_heap;
            }
            ::new (storage) T(std::forward<Arg>(arg));
            m_typeHash = TypeID<T>::Hash();
            m_vtable = VTableFor<T>();      // set last: keeps a partial box destructible-safe (m_vtable null). A ctor throw is unreachable (Astra terminates on throw); on the heap branch it would leak, not corrupt.
        }

        void CopyFrom(const AnyValue& other)
        {
            if (!other.m_vtable) return;    // stay empty
            void* storage;
            if (other.m_heap)               // same T => same inline/heap decision
            {
                m_heap = Allocate(other.m_vtable->size, other.m_vtable->align);
                storage = m_heap;
            }
            else
            {
                m_heap = nullptr;
                storage = m_inline;
            }
            other.m_vtable->copyConstruct(storage, other.Data());
            m_typeHash = other.m_typeHash;
            m_vtable = other.m_vtable;
        }

        void MoveFrom(AnyValue& other) noexcept
        {
            if (!other.m_vtable) return;    // stay empty
            if (other.m_heap)               // steal the heap block, no element move
            {
                m_heap = other.m_heap;
                m_typeHash = other.m_typeHash;
                m_vtable = other.m_vtable;
                other.m_heap = nullptr;
                other.m_vtable = nullptr;
                other.m_typeHash = 0;
            }
            else                            // inline: move-construct (nothrow by FitsInline)
            {
                m_heap = nullptr;
                other.m_vtable->moveConstruct(m_inline, other.Data());
                m_typeHash = other.m_typeHash;
                m_vtable = other.m_vtable;
                other.Reset();
            }
        }

        void Reset() noexcept
        {
            if (m_vtable)
            {
                m_vtable->destroy(Data());
                if (m_heap) Deallocate(m_heap, m_vtable->align);
            }
            m_vtable = nullptr;
            m_heap = nullptr;
            m_typeHash = 0;
        }

        alignas(kInlineAlign) std::byte m_inline[kInlineSize];
        void* m_heap = nullptr;
        const VTable* m_vtable = nullptr;
        uint64_t m_typeHash = 0;
    };
}
