#pragma once

#include <tuple>
#include <type_traits>
#include <utility>

#include "../Component/Component.hpp"
#include "../Component/ComponentRegistry.hpp"
#include "../Container/Bitmap.hpp"
#include "../Core/Base.hpp"
#include "../Core/TypeID.hpp"
#include "../Archetype/Archetype.hpp"

namespace Astra
{
    // Forward declarations
    template<typename T> struct Optional;
    template<typename T> struct Not;
    template<typename T> struct IncludeDisabled;
    template<typename... Ts> struct Any;
    template<typename... Ts> struct OneOf;
    template<typename T> struct With;

    namespace Detail
    {
        // Type trait to check if T is a query modifier
        template<typename T>
        struct IsModifier : std::false_type {};

        template<typename T>
        struct IsModifier<Optional<T>> : std::true_type {};

        template<typename T>
        struct IsModifier<Not<T>> : std::true_type {};

        // IncludeDisabled<T> is a modifier for parsing purposes (so it passes
        // ValidQueryArg and is not double-counted as a bare required component),
        // but classifies T as REQUIRED for matching -- see GetRequired below.
        template<typename T>
        struct IsModifier<IncludeDisabled<T>> : std::true_type {};
        
        template<typename... Ts>
        struct IsModifier<Any<Ts...>> : std::true_type {};
        
        template<typename... Ts>
        struct IsModifier<OneOf<Ts...>> : std::true_type {};

        template<typename T>
        struct IsModifier<With<T>> : std::true_type {};

        template<typename T>
        inline constexpr bool IsModifier_v = IsModifier<T>::value;
        
        // Extract component type from modifier or return type itself
        template<typename T>
        struct ExtractComponent
        {
            using type = T;
        };
        
        template<typename T>
        struct ExtractComponent<Optional<T>>
        {
            using type = T;
        };
        
        template<typename T>
        struct ExtractComponent<Not<T>>
        {
            using type = T;
        };

        template<typename T>
        struct ExtractComponent<IncludeDisabled<T>>
        {
            using type = T;
        };

        template<typename T>
        struct ExtractComponent<With<T>>
        {
            using type = T;
        };

        // Extract all component types from a parameter pack
        template<typename... Ts>
        struct ExtractComponents;
        
        template<>
        struct ExtractComponents<>
        {
            using type = std::tuple<>;
        };
        
        template<typename T, typename... Rest>
        struct ExtractComponents<T, Rest...>
        {
            using type = std::conditional_t<
                IsModifier_v<T>,
                typename ExtractComponents<Rest...>::type,
                decltype(std::tuple_cat(std::tuple<T>{}, std::declval<typename ExtractComponents<Rest...>::type>()))
            >;
        };
        
        // Extract components from Any/OneOf modifiers
        template<typename... Ts, typename... Rest>
        struct ExtractComponents<Any<Ts...>, Rest...>
        {
            using type = decltype(std::tuple_cat(std::tuple<Ts...>{}, std::declval<typename ExtractComponents<Rest...>::type>()));
        };
        
        template<typename... Ts, typename... Rest>
        struct ExtractComponents<OneOf<Ts...>, Rest...>
        {
            using type = decltype(std::tuple_cat(std::tuple<Ts...>{}, std::declval<typename ExtractComponents<Rest...>::type>()));
        };
        
        // Helper to get unique types from a tuple
        template<typename Tuple>
        struct UniqueTypes;
        
        template<typename... Ts>
        struct UniqueTypes<std::tuple<Ts...>>
        {
            template<typename T, typename Tuple>
            struct AddUnique;
            
            template<typename T>
            struct AddUnique<T, std::tuple<>>
            {
                using type = std::tuple<T>;
            };
            
            template<typename T, typename First, typename... Rest>
            struct AddUnique<T, std::tuple<First, Rest...>>
            {
                using type = std::conditional_t<
                    std::is_same_v<T, First>,
                    std::tuple<First, Rest...>,
                    decltype(std::tuple_cat(std::tuple<First>{}, std::declval<typename AddUnique<T, std::tuple<Rest...>>::type>()))
                >;
            };
            
            template<typename Acc, typename... Types>
            struct Accumulate;
            
            template<typename Acc>
            struct Accumulate<Acc>
            {
                using type = Acc;
            };
            
            template<typename Acc, typename T, typename... Types>
            struct Accumulate<Acc, T, Types...>
            {
                using type = typename Accumulate<typename AddUnique<T, Acc>::type, Types...>::type;
            };
            
            using type = typename Accumulate<std::tuple<>, Ts...>::type;
        };
        
        // Get all actual component types (including from modifiers)
        template<typename... QueryArgs>
        using AllComponents = typename UniqueTypes<typename ExtractComponents<QueryArgs...>::type>::type;
        
        // Separate query arguments into categories
        template<typename... QueryArgs>
        struct QueryClassifier
        {
            // Helper to filter by modifier type
            template<template<typename...> class Modifier, typename... Args>
            struct FilterByModifier;
            
            template<template<typename...> class Modifier>
            struct FilterByModifier<Modifier>
            {
                using type = std::tuple<>;
            };
            
            template<template<typename...> class Modifier, typename T, typename... Rest>
            struct FilterByModifier<Modifier, T, Rest...>
            {
                using type = typename FilterByModifier<Modifier, Rest...>::type;
            };
            
            template<template<typename...> class Modifier, typename T, typename... Rest>
            struct FilterByModifier<Modifier, Modifier<T>, Rest...>
            {
                using type = decltype(std::tuple_cat(std::tuple<T>{}, std::declval<typename FilterByModifier<Modifier, Rest...>::type>()));
            };
            
            // Special handling for Any/OneOf with multiple components
            template<template<typename...> class Modifier, typename... Ts, typename... Rest>
            struct FilterByModifier<Modifier, Modifier<Ts...>, Rest...>
            {
                using type = decltype(std::tuple_cat(std::tuple<std::tuple<Ts...>>{}, std::declval<typename FilterByModifier<Modifier, Rest...>::type>()));
            };
            
            // Get required components (non-modified)
            // Primary template handles the empty pack; explicit specialization in
            // class scope is ill-formed for gcc/clang (MSVC permits it).
            template<typename... Args>
            struct GetRequired
            {
                using type = std::tuple<>;
            };

            template<typename T, typename... Rest>
            struct GetRequired<T, Rest...>
            {
                using type = std::conditional_t<
                    IsModifier_v<T>,
                    typename GetRequired<Rest...>::type,
                    decltype(std::tuple_cat(std::tuple<T>{}, std::declval<typename GetRequired<Rest...>::type>()))
                >;
            };

            // IncludeDisabled<T> is required-for-matching (T must be present and is
            // passed by reference to the callback) but is excluded from enabled
            // filtering. More specialised than the generic (modifier-skipping) case,
            // so it wins; T lands in RequiredComponents exactly like a bare required
            // component, and the enabled-filter set is built separately from the raw
            // query args so this T never enters it.
            template<typename T, typename... Rest>
            struct GetRequired<IncludeDisabled<T>, Rest...>
            {
                using type = decltype(std::tuple_cat(std::tuple<T>{}, std::declval<typename GetRequired<Rest...>::type>()));
            };

            using RequiredComponents = typename GetRequired<QueryArgs...>::type;
            using OptionalComponents = typename FilterByModifier<Optional, QueryArgs...>::type;
            using ExcludedComponents = typename FilterByModifier<Not, QueryArgs...>::type;
            using AnyGroups = typename FilterByModifier<Any, QueryArgs...>::type;
            using OneOfGroups = typename FilterByModifier<OneOf, QueryArgs...>::type;
            using WithComponents     = typename FilterByModifier<With, QueryArgs...>::type;
        };

        // ============ Enableable-components query-filter type sets (spec §5) ============
        //
        // The enabled-filter set of a view = required + optional components that
        // are IsEnableableV and NOT wrapped in IncludeDisabled. Built directly from
        // the raw query args (for required) and from the already-classified optional
        // tuple. When both sets are empty the view compiles to the pre-existing,
        // byte-identical iteration loops (invariant 1: zero cost when unused).

        // Bare (unwrapped) required components that are enableable. A modifier --
        // Optional/Not/Any/OneOf/IncludeDisabled -- is skipped, so IncludeDisabled<T>
        // is automatically excluded (its T never appears here => no filtering). This
        // set drives per-chunk run extraction and chunk skipping.
        template<typename... QueryArgs>
        struct EnableableRequiredFilter
        {
            using type = decltype(std::tuple_cat(
                std::conditional_t<(!IsModifier_v<QueryArgs> && IsEnableableV<QueryArgs>),
                                   std::tuple<QueryArgs>, std::tuple<>>{}...));
        };
        template<typename... QueryArgs>
        using EnableableRequiredFilter_t = typename EnableableRequiredFilter<QueryArgs...>::type;

        // Filter a component tuple down to its enableable members. Applied to the
        // Optional tuple: those optionals whose pointer is nulled per entity while
        // disabled.
        template<typename Tuple>
        struct FilterEnableable;
        template<typename... Ts>
        struct FilterEnableable<std::tuple<Ts...>>
        {
            using type = decltype(std::tuple_cat(
                std::conditional_t<IsEnableableV<Ts>, std::tuple<Ts>, std::tuple<>>{}...));
        };
        template<typename Tuple>
        using FilterEnableable_t = typename FilterEnableable<Tuple>::type;

        // Per-query-arg read/write classification for ViewAccess.
        // Default (bare data arg): const T -> read, T -> write.
        template<typename Arg>
        struct ArgAccess
        {
            using Read  = std::conditional_t<std::is_const_v<Arg>, std::tuple<std::remove_const_t<Arg>>, std::tuple<>>;
            using Write = std::conditional_t<std::is_const_v<Arg>, std::tuple<>, std::tuple<Arg>>;
        };
        // Optional<T>: yields a pointer; const T -> read, T -> write.
        template<typename T>
        struct ArgAccess<Optional<T>>
        {
            using Read  = std::conditional_t<std::is_const_v<T>, std::tuple<std::remove_const_t<T>>, std::tuple<>>;
            using Write = std::conditional_t<std::is_const_v<T>, std::tuple<>, std::tuple<T>>;
        };
        // IncludeDisabled<T>: yielded mutable (see GetRequired) -> write.
        template<typename T>
        struct ArgAccess<IncludeDisabled<T>>
        {
            using Read  = std::tuple<>;
            using Write = std::tuple<T>;
        };
        // Match-only / grouping modifiers: zero access footprint.
        template<typename T>    struct ArgAccess<With<T>> { using Read = std::tuple<>; using Write = std::tuple<>; };
        template<typename T>    struct ArgAccess<Not<T>>  { using Read = std::tuple<>; using Write = std::tuple<>; };
        template<typename... T> struct ArgAccess<Any<T...>>   { using Read = std::tuple<>; using Write = std::tuple<>; };
        template<typename... T> struct ArgAccess<OneOf<T...>> { using Read = std::tuple<>; using Write = std::tuple<>; };

        template<typename... Args>
        struct AccessReads  { using type = decltype(std::tuple_cat(std::declval<typename ArgAccess<Args>::Read>()...)); };
        template<typename... Args>
        struct AccessWrites { using type = decltype(std::tuple_cat(std::declval<typename ArgAccess<Args>::Write>()...)); };
    }
    
    // Query modifier types
    template<typename T>
    struct Optional
    {
        static_assert(Component<T>, "Optional can only be used with valid components");
    };
    
    template<typename T>
    struct Not
    {
        static_assert(Component<T>, "Not can only be used with valid components");
    };

    // Match-only filter: T must be present on the archetype for a match, but T is
    // NOT yielded to the callback and carries zero access footprint for scheduling
    // (ViewAccess ignores it). The positive twin of Not<T>.
    template<typename T>
    struct With
    {
        static_assert(Component<T>, "With can only be used with valid components");
    };

    // View modifier: opt a REQUIRED enableable component OUT of enabled-only
    // filtering. `CreateView<IncludeDisabled<T>>` matches (and hands the callback)
    // every entity that has T, disabled ones included -- the DOTS "include
    // disabled" query. T is still required for matching; it is only removed from
    // the per-view enabled-filter set (spec §5).
    template<typename T>
    struct IncludeDisabled
    {
        static_assert(Component<T>, "IncludeDisabled can only be used with valid components");
        static_assert(IsEnableableV<T>,
            "IncludeDisabled<T> requires an enableable component (opt in with `static constexpr bool AstraEnableable = true;`); "
            "a non-enableable component is never enabled-filtered, so opting it out is meaningless");
    };

    template<typename... Ts>
    struct Any
    {
        static_assert((Component<Ts> && ...), "Any can only be used with valid components");
        static_assert(sizeof...(Ts) > 0, "Any must have at least one component");
    };
    
    template<typename... Ts>
    struct OneOf
    {
        static_assert((Component<Ts> && ...), "OneOf can only be used with valid components");
        static_assert(sizeof...(Ts) > 1, "OneOf must have at least two components");
    };
    
    // Query builder that processes modifiers and creates masks
    template<typename... QueryArgs>
    class QueryBuilder
    {
    private:
        using Classifier = Detail::QueryClassifier<QueryArgs...>;
        
        template<typename Tuple>
        static ComponentMask MakeMaskFromTuple()
        {
            return []<typename... Ts>(std::tuple<Ts...>*)
            {
                return MakeComponentMask<Ts...>();
            }(static_cast<Tuple*>(nullptr));
        }
        
    public:
        // Get mask for required components
        static ComponentMask GetRequiredMask()
        {
            return MakeMaskFromTuple<typename Classifier::RequiredComponents>();
        }
        
        // Get mask for optional components
        static ComponentMask GetOptionalMask()
        {
            return MakeMaskFromTuple<typename Classifier::OptionalComponents>();
        }
        
        // Get mask for excluded components
        static ComponentMask GetExcludedMask()
        {
            return MakeMaskFromTuple<typename Classifier::ExcludedComponents>();
        }

        // Match-only components (With<T>): required for matching, never yielded.
        static ComponentMask GetWithMask()
        {
            return MakeMaskFromTuple<typename Classifier::WithComponents>();
        }

        // Handle Any groups - must have at least one component from each group
        template<typename Tuple, size_t... Is>
        static bool CheckAnyGroups(const ComponentMask& archetypeMask, std::index_sequence<Is...>)
        {
            return (CheckSingleAnyGroup<Is>(archetypeMask) && ...);
        }
        
        template<size_t I>
        static bool CheckSingleAnyGroup(const ComponentMask& archetypeMask)
        {
            using Groups = typename Classifier::AnyGroups;
            if constexpr (I < std::tuple_size_v<Groups>)
            {
                using Group = std::tuple_element_t<I, Groups>;
                return []<typename... Ts>(const ComponentMask& mask, std::tuple<Ts...>*)
                {
                    ComponentMask anyMask = MakeComponentMask<Ts...>();
                    return (mask & anyMask).Any();
                }(archetypeMask, static_cast<Group*>(nullptr));
            }
            return true;
        }
        
        // Handle OneOf groups - must have exactly one component from each group
        template<typename Tuple, size_t... Is>
        static bool CheckOneOfGroups(const ComponentMask& archetypeMask, std::index_sequence<Is...>)
        {
            return (CheckSingleOneOfGroup<Is>(archetypeMask) && ...);
        }
        
        template<size_t I>
        static bool CheckSingleOneOfGroup(const ComponentMask& archetypeMask)
        {
            using Groups = typename Classifier::OneOfGroups;
            if constexpr (I < std::tuple_size_v<Groups>)
            {
                using Group = std::tuple_element_t<I, Groups>;
                return []<typename... Ts>(const ComponentMask& mask, std::tuple<Ts...>*)
                {
                    size_t count = 0;
                    ((mask.Test(TypeID<Ts>::Value()) ? ++count : 0), ...);
                    return count == 1;
                }(archetypeMask, static_cast<Group*>(nullptr));
            }
            return true;
        }
        
        // Check if archetype matches this query
        static bool Matches(const ComponentMask& archetypeMask)
        {
            // Must have all required AND all With components
            if (!archetypeMask.HasAll(GetRequiredMask() | GetWithMask()))
                return false;
            
            // Must NOT have any excluded components
            auto excluded = GetExcludedMask();
            if ((archetypeMask & excluded).Any())
                return false;
            
            // Check Any groups
            constexpr size_t anyGroupCount = std::tuple_size_v<typename Classifier::AnyGroups>;
            if constexpr (anyGroupCount > 0)
            {
                if (!CheckAnyGroups<typename Classifier::AnyGroups>(archetypeMask, std::make_index_sequence<anyGroupCount>{}))
                {
                    return false;
                }
            }
            
            // Check OneOf groups
            constexpr size_t oneOfGroupCount = std::tuple_size_v<typename Classifier::OneOfGroups>;
            if constexpr (oneOfGroupCount > 0)
            {
                if (!CheckOneOfGroups<typename Classifier::OneOfGroups>(archetypeMask, std::make_index_sequence<oneOfGroupCount>{}))
                {
                    return false;
                }
            }
            
            return true;
        }
    };
    
    // Concept to validate query arguments
    template<typename T>
    concept ValidQueryArg = Component<T> || Detail::IsModifier_v<T>;
    
    // Concept for a valid query (all arguments must be valid)
    template<typename... Args>
    concept ValidQuery = (ValidQueryArg<Args> && ...);
}
