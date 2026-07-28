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

        // ---- Callable-signature deduction for the registration concepts -----
        template<typename> struct FnSignature;                                   // undefined base
        template<typename R, typename C, typename... A>
        struct FnSignature<R(C::*)(A...)>       { using Params = std::tuple<A...>; };
        template<typename R, typename C, typename... A>
        struct FnSignature<R(C::*)(A...) const> { using Params = std::tuple<A...>; };
        template<typename R, typename... A>
        struct FnSignature<R(*)(A...)>          { using Params = std::tuple<A...>; };

        // Non-empty and every element is a SystemParam.
        template<typename Tuple> struct AllParams : std::false_type {};
        template<typename... A> struct AllParams<std::tuple<A...>>
            : std::bool_constant<(sizeof...(A) > 0) && (IsSystemParam_v<A> && ...)> {};

        // Functor whose operator() params are all SystemParams.
        template<typename Fn, typename = void>
        struct IsParamFunctor : std::false_type {};
        template<typename Fn>
        struct IsParamFunctor<Fn, std::void_t<decltype(&Fn::operator())>>
            : AllParams<typename FnSignature<decltype(&Fn::operator())>::Params> {};
        template<typename Fn> inline constexpr bool IsParamFunctor_v = IsParamFunctor<Fn>::value;

        // Free-function pointer type whose params are all SystemParams. Only the
        // R(*)(A...) partial specialization is truthy; anything else is false
        // (no hard error on non-function types).
        template<typename FnType> struct IsParamFreeFunction : std::false_type {};
        template<typename R, typename... A>
        struct IsParamFreeFunction<R(*)(A...)> : AllParams<std::tuple<A...>> {};
        template<typename FnType> inline constexpr bool IsParamFreeFunction_v = IsParamFreeFunction<FnType>::value;
    }

    // A callable whose operator() parameters are all SystemParams (View&/Res/
    // ResMut/Commands), non-empty. Disjoint from ContextSystem (whose lone
    // SystemContext& arg is not a SystemParam) and from view-lambdas.
    template<typename T>
    concept ParamFunctor = Detail::IsParamFunctor_v<std::decay_t<T>>;

    // Shared machinery for a parameter-function system (design §4.2). Task 3
    // added the harvested-access typedefs; this task adds Run/BuildParam and
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

    protected:
        // Rebind caches if the registry changed; run the resource-presence gate;
        // then invoke `invoke(BuildParam<Params>(ctx, reg)...)`. `invoke` is the
        // wrapper's thunk that forwards to the user fn / free fn.
        template<typename Invoke>
        void Run(SystemContext& ctx, Invoke invoke)
        {
            Registry& reg = ctx.GetRegistry();
            if (m_viewRegistry != &reg)   // IM-23 parity: rebuild views on registry switch
            {
                ResetViews(std::index_sequence_for<Params...>{});
                m_viewRegistry = &reg;
            }
            if (!ResourcesPresent(reg))   // skip-and-log (design §6)
            {
                if (!m_loggedMissing)
                {
                    ASTRA_LOG_ERROR("Astra param-system skipped this frame: a declared "
                                    "Res/ResMut resource is absent from the Registry.");
                    m_loggedMissing = true;
                }
                return;
            }
            m_loggedMissing = false;      // a later disappearance logs again
            InvokeImpl(ctx, reg, invoke, std::index_sequence_for<Params...>{});
        }

    private:
        template<typename Invoke, size_t... Is>
        void InvokeImpl(SystemContext& ctx, Registry& reg, Invoke& invoke, std::index_sequence<Is...>)
        {
            invoke(BuildParam<Params, Is>(ctx, reg)...);
        }

        // Construct one parameter. View params return an lvalue reference into
        // the persistent cache; Res/ResMut/Commands return a fresh handle.
        template<typename P, size_t I>
        decltype(auto) BuildParam(SystemContext& ctx, Registry& reg)
        {
            if constexpr (Detail::IsView<P>::value)
            {
                auto& slot = std::get<I>(m_views);       // std::optional<View<A...>>
                if (!slot.has_value())
                    slot.emplace(CreateViewFor(reg, static_cast<typename Detail::ViewOf<P>::type*>(nullptr)));
                return static_cast<P>(*slot);            // P == View<A...>& -> lvalue ref
            }
            else if constexpr (std::is_same_v<P, Commands>)
            {
                return Commands{ctx};
            }
            else if constexpr (Detail::IsResourceParam<P>::value)
            {
                using RT = typename Detail::ResourceType<P>::type;
                if constexpr (std::is_same_v<P, Res<RT>>)
                    return Res<RT>{reg.GetResource<RT>()};
                else
                    return ResMut<RT>{reg.GetResource<RT>()};
            }
        }

        template<typename... A>
        static View<A...> CreateViewFor(Registry& reg, View<A...>*) { return reg.CreateView<A...>(); }

        bool ResourcesPresent(Registry& reg) const
        {
            return (ParamPresent<Params>(reg) && ...);
        }
        template<typename P>
        static bool ParamPresent(Registry& reg)
        {
            if constexpr (Detail::IsResourceParam<P>::value)
                return reg.GetResource<typename Detail::ResourceType<P>::type>() != nullptr;
            else
                return true;
        }

        template<size_t... Is>
        void ResetViews(std::index_sequence<Is...>) { (ResetSlot(std::get<Is>(m_views)), ...); }
        template<typename Slot>
        static void ResetSlot(Slot& slot)
        {
            if constexpr (!std::is_same_v<Slot, std::monostate>)
                slot.reset();
        }

        // One cache slot per parameter, index-aligned with Params (monostate for
        // non-View params). IM-23 view caching, generalized to N views.
        std::tuple<typename Detail::ViewSlot<Params>::type...> m_views{};
        Registry* m_viewRegistry = nullptr;
        bool m_loggedMissing = false;
    };

    // Lambda / functor param-system (registered by value). Fn is the closure
    // type (unique per lambda -> unique wrapper type -> unique TypeID hash).
    template<typename Fn, typename... Params>
    class FunctionSystemWrapper : public SystemParamBinder<Params...>
    {
        Fn m_fn;
    public:
        explicit FunctionSystemWrapper(Fn fn) : m_fn(std::move(fn)) {}
        void operator()(SystemContext& ctx)
        {
            this->Run(ctx, [this](auto&&... p) { m_fn(static_cast<decltype(p)>(p)...); });
        }
    };

    // Free-function param-system (registered as a non-type template argument, so
    // each function is its own wrapper type -> no same-signature hash collision).
    // No stored callable: FnPtr is the template argument.
    template<auto FnPtr, typename... Params>
    class FreeFunctionSystemWrapper : public SystemParamBinder<Params...>
    {
    public:
        void operator()(SystemContext& ctx)
        {
            this->Run(ctx, [](auto&&... p) { FnPtr(static_cast<decltype(p)>(p)...); });
        }
    };
}
