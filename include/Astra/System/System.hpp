#pragma once

#include <concepts>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../Component/Component.hpp"
#include "../Core/Base.hpp"
#include "../Registry/Registry.hpp"
#include "SystemContext.hpp"
#include "SystemParam.hpp"

namespace Astra
{
    template<typename T>
    concept System = requires(T system, Registry& registry)
    {
        { system(registry) } -> std::same_as<void>;
    };
    
    template<typename... Components>
    struct Reads { using type = std::tuple<Components...>; };

    template<typename... Components>
    struct Writes { using type = std::tuple<Components...>; };

    template<typename... Systems>
    struct Before { using type = std::tuple<Systems...>; };

    template<typename... Systems>
    struct After { using type = std::tuple<Systems...>; };

    template<typename... Systems>
    struct AmbiguousWith { using type = std::tuple<Systems...>; };

    template<typename... Resources>
    struct ReadsResources { using type = std::tuple<Resources...>; };

    template<typename... Resources>
    struct WritesResources { using type = std::tuple<Resources...>; };

    // Per-resource concurrency trait. A resource whose ConcurrentReadSafe is
    // false serializes ALL access to it -- even two readers -- for genuinely
    // non-thread-safe external state (a GPU queue, a non-thread-safe library
    // handle). Specialize to opt out; the default is safe.
    template<typename T>
    struct ResourceTraits { static constexpr bool ConcurrentReadSafe = true; };

    // Marker: a system that mutates entity structure (create/destroy/add/remove)
    // or accesses state outside its declared masks. Forces a solo execution group.
    struct Exclusive {};

    namespace Detail
    {
        template<typename T> struct TraitReads  { using type = std::tuple<>; };
        template<typename... R> struct TraitReads<Reads<R...>>  { using type = std::tuple<R...>; };
        template<typename T> struct TraitWrites { using type = std::tuple<>; };
        template<typename... W> struct TraitWrites<Writes<W...>> { using type = std::tuple<W...>; };

        template<typename T> struct TraitBefore { using type = std::tuple<>; };
        template<typename... S> struct TraitBefore<Before<S...>> { using type = std::tuple<S...>; };
        template<typename T> struct TraitAfter  { using type = std::tuple<>; };
        template<typename... S> struct TraitAfter<After<S...>>   { using type = std::tuple<S...>; };
        template<typename T> struct TraitAmbiguousWith { using type = std::tuple<>; };
        template<typename... S> struct TraitAmbiguousWith<AmbiguousWith<S...>> { using type = std::tuple<S...>; };

        template<typename T> struct TraitReadsResources  { using type = std::tuple<>; };
        template<typename... R> struct TraitReadsResources<ReadsResources<R...>>  { using type = std::tuple<R...>; };
        template<typename T> struct TraitWritesResources { using type = std::tuple<>; };
        template<typename... W> struct TraitWritesResources<WritesResources<W...>> { using type = std::tuple<W...>; };
    }

    // Accepts Reads<...>, Writes<...>, and Exclusive in any order/combination.
    template<typename... Traits>
    struct SystemTraits
    {
        using ReadsComponents  = decltype(std::tuple_cat(std::declval<typename Detail::TraitReads<Traits>::type>()...));
        using WritesComponents = decltype(std::tuple_cat(std::declval<typename Detail::TraitWrites<Traits>::type>()...));
        using BeforeTypes        = decltype(std::tuple_cat(std::declval<typename Detail::TraitBefore<Traits>::type>()...));
        using AfterTypes         = decltype(std::tuple_cat(std::declval<typename Detail::TraitAfter<Traits>::type>()...));
        using AmbiguousWithTypes = decltype(std::tuple_cat(std::declval<typename Detail::TraitAmbiguousWith<Traits>::type>()...));
        using ReadsResourceTypes  = decltype(std::tuple_cat(std::declval<typename Detail::TraitReadsResources<Traits>::type>()...));
        using WritesResourceTypes = decltype(std::tuple_cat(std::declval<typename Detail::TraitWritesResources<Traits>::type>()...));
        static constexpr bool HasTraits = true;
        static constexpr bool RequiresExclusive = (std::is_same_v<Traits, Exclusive> || ...);
    };

    template<typename T>
    struct HasSystemTraits : std::false_type {};
    
    template<typename T>
    requires requires { typename T::ReadsComponents; typename T::WritesComponents; T::HasTraits; }
    struct HasSystemTraits<T> : std::true_type {};
    
    template<typename T>
    inline constexpr bool HasSystemTraits_v = HasSystemTraits<T>::value;
    
    // NOTE (Task 2): a void(SystemContext&) callable has an operator() and is
    // NOT invocable with Registry&, so without the trailing !ContextSystem<T>
    // clause it would satisfy LambdaLike and be misrouted into the
    // view-lambda ExtractAndExecute path below (which expects
    // (Entity, Components&...), not (SystemContext&)) -- a hard compile
    // error inside LambdaSystemWrapper, not a graceful fallback. Context
    // systems are routed to their own AddSystem overload instead (see
    // SystemScheduler::AddSystem / SystemContext.hpp).
    template<typename T>
    concept LambdaLike = requires
    {
        &T::operator();  // Has operator()
    } && !std::invocable<T, Registry&>    // But not a traditional system
      && !ContextSystem<T>                // ...and not a void(SystemContext&) context system
      && !ParamFunctor<T>;                // ...and not a View/Res/ResMut/Commands param-function

    template<typename Lambda, typename... Args>
    class LambdaSystemWrapper
    {
        template<typename First, typename... Rest>
        struct SkipEntityArg
        {
            using Components = std::tuple<Rest...>;
        };

        template<typename T>
        static constexpr bool IsReadOnly = std::is_const_v<std::remove_reference_t<T>>;

        template<typename T>
        using BaseType = std::remove_const_t<std::remove_reference_t<T>>;

        template<typename Tuple, size_t... Is>
        static auto ExtractReads(std::index_sequence<Is...>)
        {
            return std::tuple_cat(
                std::conditional_t<IsReadOnly<std::tuple_element_t<Is, Tuple>>,
                    std::tuple<BaseType<std::tuple_element_t<Is, Tuple>>>,
                        std::tuple<>
                >{}...
            );
        }

        template<typename Tuple, size_t... Is>
        static auto ExtractWrites(std::index_sequence<Is...>)
        {
            return std::tuple_cat(
                std::conditional_t<!IsReadOnly<std::tuple_element_t<Is, Tuple>>,
                    std::tuple<BaseType<std::tuple_element_t<Is, Tuple>>>,
                        std::tuple<>
                >{}...
            );
        }

    public:
        using ComponentArgs = typename SkipEntityArg<Args...>::Components;

        // Every component parameter must be an lvalue reference ('T&' or
        // 'const T&'). IsReadOnly's const-ness inference above is only
        // correct for reference params: a 'const Position*' yields
        // is_const_v<const Position*> == false (the POINTER is non-const,
        // only the pointee is), so a pointer param is silently mis-inferred
        // as a WRITE and builds a CreateView over the bogus type
        // 'const Position*' -- which no entity ever has -- so the lambda
        // silently never iterates instead of failing to compile. Reject
        // that up front with a clear message. An empty ComponentArgs (a
        // lambda with only Entity) makes the fold vacuously true: no
        // component params, nothing to reject.
        template<typename Tuple> struct AllLvalueRefs;
        template<typename... Ts> struct AllLvalueRefs<std::tuple<Ts...>>
            : std::bool_constant<(std::is_lvalue_reference_v<Ts> && ...)> {};

        static_assert(AllLvalueRefs<ComponentArgs>::value,
            "Astra system lambda component parameters must be 'T&' or 'const T&'. "
            "Pointer, by-value, and optional/nullable component parameters are not "
            "supported (a 'const T*' would be mis-inferred as a write and build a "
            "malformed view). Use 'const T&' for read access or 'T&' for write access.");

        using ReadsComponents = decltype(ExtractReads<ComponentArgs>(std::make_index_sequence<std::tuple_size_v<ComponentArgs>>{}));
        using WritesComponents = decltype(ExtractWrites<ComponentArgs>(std::make_index_sequence<std::tuple_size_v<ComponentArgs>>{}));
        static constexpr bool HasTraits = true;
        static constexpr bool RequiresExclusive = false;  // lambda systems operate on a view; never exclusive

        explicit LambdaSystemWrapper(Lambda lambda) : m_lambda(std::move(lambda)) {}
        
        // Implement the System interface
        void operator()(Registry& registry)
        {
            // Extract component types without const/ref
            ExtractAndExecute<Args...>(registry);
        }

    private:
        template<typename First, typename... Components>
        void ExtractAndExecute(Registry& registry)
        {
            static_assert(std::is_same_v<BaseType<First>, Entity>, "First parameter must be Entity");

            // IM-23: build the View once (const-ness of components preserved:
            // const T& -> const T, T& -> T) and PERSIST it across Execute()
            // calls. The View's ctor collects matching archetypes from
            // generation 0 (full scan + match-test + sort); creating a fresh
            // View every frame -- as this did before -- threw that away each
            // call. Kept, its EnsureArchetypes()/m_lastGeneration incremental
            // refresh only appends newly-created matching archetypes (or fully
            // rebuilds after archetype removals), matching a fresh collect. The
            // iterated entity set is therefore identical to the old per-frame
            // View; only the per-frame rebuild cost is removed.
            //
            // Rebuild if the Registry differs from the one the View was built
            // against (the scheduler supports switching registries between
            // Execute() calls -- see SystemScheduler::Execute's command-buffer
            // rebind; this mirrors that pointer-identity check).
            if (!m_view || m_viewRegistry != &registry)
            {
                m_view.emplace(registry.CreateView<
                    std::conditional_t<IsReadOnly<Components>,
                        const BaseType<Components>,
                            BaseType<Components>
                    >...
                >());
                m_viewRegistry = &registry;
            }
            m_view->ForEach(m_lambda);
        }

        // IM-23: the View type this system iterates -- identical to what the
        // CreateView<...> call above returns -- computed once from the lambda's
        // component parameters so it can be held as a persistent member.
        template<typename Tuple, size_t... Is>
        static auto ComputeViewType(std::index_sequence<Is...>)
            -> View<std::conditional_t<IsReadOnly<std::tuple_element_t<Is, Tuple>>,
                        const BaseType<std::tuple_element_t<Is, Tuple>>,
                        BaseType<std::tuple_element_t<Is, Tuple>>>...>;
        using ViewType = decltype(ComputeViewType<ComponentArgs>(
            std::make_index_sequence<std::tuple_size_v<ComponentArgs>>{}));

        Lambda m_lambda;
        std::optional<ViewType> m_view;      // persistent View; built lazily on first Execute
        Registry* m_viewRegistry = nullptr;  // registry m_view was built against; rebuild if it changes
    };
}
