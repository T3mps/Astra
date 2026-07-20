#pragma once

// Mosaic::FunctionRef<Sig> -- a non-owning, zero-allocation callable VIEW
// (void* + thunk). For SYNCHRONOUS, non-escaping callbacks only: the referent
// MUST outlive the FunctionRef -- do NOT store one (it would dangle); a stored
// callback needs an owning std::function (or a Delegate). C++26
// std::function_ref-aligned; replaceable by a using-alias when it ships.
//
// The one canonical Starworks copy (Manifold2D and the host engine each carried
// their own). The IWorkScheduler seam takes its callback by this type.

#include <cassert>
#include <memory>
#include <type_traits>

namespace Mosaic
{
    template <class Sig> class FunctionRef;

    template <class R, class... Args>
    class FunctionRef<R(Args...)>
    {
        void* m_obj = nullptr;
        R (*m_thunk)(void*, Args...) = nullptr;

    public:
        FunctionRef() = default;

        // Implicit by design: call sites pass a lambda directly.
        template <class F>
            requires (!std::is_same_v<std::remove_cvref_t<F>, FunctionRef>
                      && std::is_invocable_r_v<R, F&, Args...>)
        FunctionRef(F&& f) noexcept
            // For a raw function name, F deduces as a function TYPE and addressof(f)
            // yields the function's stable address (not a temporary) -- safe. Same for
            // any lvalue callable. Do NOT bind a function-POINTER rvalue (e.g. a cast
            // result): F would deduce as a pointer type and m_obj would dangle.
            : m_obj(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
              m_thunk(+[](void* o, Args... a) -> R {
                  return (*static_cast<std::remove_reference_t<F>*>(o))(static_cast<Args&&>(a)...);
              })
        {
        }

        R operator()(Args... a) const
        {
            // Calling through an empty (default-constructed / moved-from) FunctionRef
            // dereferences a null thunk -- UB. Debug-assert on the bool conversion;
            // the release path compiles out under NDEBUG. Callers must check
            // operator bool if emptiness is reachable.
            assert(static_cast<bool>(*this) && "Mosaic::FunctionRef: call through an empty FunctionRef");
            return m_thunk(m_obj, static_cast<Args&&>(a)...);
        }

        explicit operator bool() const noexcept { return m_thunk != nullptr; }
    };
}
