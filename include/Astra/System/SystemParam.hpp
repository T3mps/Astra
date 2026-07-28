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

    namespace Detail
    {
        // ---- Per-parameter access classification (design §4.1) --------------
        // Default (Commands and anything else): contributes nothing.
        template<typename P> struct ParamAccess
        {
            using Reads    = std::tuple<>;  using Writes    = std::tuple<>;
            using ResReads = std::tuple<>;  using ResWrites = std::tuple<>;
        };
        template<typename... A> struct ParamAccess<View<A...>&>
        {
            using Reads    = typename ViewAccess<View<A...>>::Reads;   // consumes Stage 1
            using Writes   = typename ViewAccess<View<A...>>::Writes;
            using ResReads = std::tuple<>;  using ResWrites = std::tuple<>;
        };
        template<typename T> struct ParamAccess<Res<T>>
        {
            using Reads    = std::tuple<>;  using Writes    = std::tuple<>;
            using ResReads = std::tuple<T>; using ResWrites = std::tuple<>;
        };
        template<typename T> struct ParamAccess<ResMut<T>>
        {
            using Reads    = std::tuple<>;  using Writes    = std::tuple<>;
            using ResReads = std::tuple<>;  using ResWrites = std::tuple<T>;
        };

        // ---- IsSystemParam: the closed param set (design §5.1) --------------
        template<typename P> struct IsSystemParam : std::false_type {};
        template<typename... A> struct IsSystemParam<View<A...>&> : std::true_type {};
        template<typename T>    struct IsSystemParam<Res<T>>      : std::true_type {};
        template<typename T>    struct IsSystemParam<ResMut<T>>   : std::true_type {};
        template<>              struct IsSystemParam<Commands>    : std::true_type {};
        template<typename P> inline constexpr bool IsSystemParam_v = IsSystemParam<P>::value;

        // ---- Shape helpers used by the binder (Task 3/4) --------------------
        template<typename P> struct IsView : std::false_type {};
        template<typename... A> struct IsView<View<A...>&> : std::true_type {};

        template<typename P> struct IsResourceParam : std::false_type {};
        template<typename T> struct IsResourceParam<Res<T>>    : std::true_type {};
        template<typename T> struct IsResourceParam<ResMut<T>> : std::true_type {};

        template<typename P> struct ResourceType;                 // Res<T>/ResMut<T> -> T
        template<typename T> struct ResourceType<Res<T>>    { using type = T; };
        template<typename T> struct ResourceType<ResMut<T>> { using type = T; };

        template<typename P> struct ViewOf { using type = void; };            // View<A...>& -> View<A...>
        template<typename... A> struct ViewOf<View<A...>&> { using type = View<A...>; };

        template<typename P> struct ViewSlot { using type = std::monostate; };   // per-param cache slot
        template<typename... A> struct ViewSlot<View<A...>&> { using type = std::optional<View<A...>>; };
    }

    // Shared machinery for a parameter-function system (design §4.2). This task
    // adds only the harvested-access typedefs; Task 4 adds Run/BuildParam and
    // the per-View cache. The typedefs make HasSystemTraits_v<Wrapper> true so
    // the existing ExtractSystemTraits fills the scheduler's masks unchanged.
    template<typename... Params>
    class SystemParamBinder
    {
    public:
        using ReadsComponents     = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::Reads>()...));
        using WritesComponents    = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::Writes>()...));
        using ReadsResourceTypes  = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::ResReads>()...));
        using WritesResourceTypes = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::ResWrites>()...));
        static constexpr bool HasTraits = true;
        static constexpr bool RequiresExclusive = false;
    };
}
