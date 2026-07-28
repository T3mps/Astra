#pragma once

#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "../Commands/CommandBuffer.hpp"
#include "../Core/Base.hpp"
#include "../Core/Log.hpp"
#include "../Registry/Registry.hpp"
#include "../Registry/View.hpp"
#include "SystemContext.hpp"

namespace Astra
{
    // ---- SystemParam handle types (design §3.1) --------------------------------

    // Read-only resource handle. Contributes T to a system's resourceReads.
    // Never null inside a system body: SystemParamBinder's resource-presence
    // gate (Task 4) skips-and-logs before invoking if the resource is absent.
    template<typename T>
    class Res
    {
        const T* m_ptr;
    public:
        explicit Res(const T* p) noexcept : m_ptr(p) {}
        ASTRA_NODISCARD const T& operator*()  const noexcept { return *m_ptr; }
        ASTRA_NODISCARD const T* operator->() const noexcept { return  m_ptr; }
        ASTRA_NODISCARD const T& Get()        const noexcept { return *m_ptr; }
    };

    // Mutable resource handle. Contributes T to a system's resourceWrites.
    template<typename T>
    class ResMut
    {
        T* m_ptr;
    public:
        explicit ResMut(T* p) noexcept : m_ptr(p) {}
        ASTRA_NODISCARD T& operator*()  const noexcept { return *m_ptr; }
        ASTRA_NODISCARD T* operator->() const noexcept { return  m_ptr; }
        ASTRA_NODISCARD T& Get()        const noexcept { return *m_ptr; }
    };

    // Deferred structural-change handle. Contributes NO scheduling access
    // (per-worker buffer, deterministic flush at sync points). Each operator->
    // re-stamps the sort key via SystemContext::Commands(), so N statements
    // record N deterministically-ordered commands. Do NOT cache the returned
    // pointer across statements. Ctor is explicit so a `(Commands)` lambda is
    // NOT implicitly convertible from SystemContext& (keeps ContextSystem and
    // ParamFunctor disjoint).
    class Commands
    {
        SystemContext* m_ctx;
    public:
        explicit Commands(SystemContext& ctx) noexcept : m_ctx(&ctx) {}
        ASTRA_NODISCARD CommandBuffer* operator->() const { return &m_ctx->Commands(); }
        ASTRA_NODISCARD CommandBuffer& Get()        const { return  m_ctx->Commands(); }
    };
}
