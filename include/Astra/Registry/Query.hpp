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
    // Forward declaration of MakeComponentMask from Archetype.hpp
    template<Component... Components>
    ASTRA_NODISCARD constexpr ComponentMask MakeComponentMask() noexcept;
    
    // Forward declarations
    template<typename T> struct Optional;
    template<typename T> struct Not;
    template<typename... Ts> struct Any;
    template<typename... Ts> struct OneOf;
    
    namespace Detail
    {
        // Type trait to check if T is a query modifier
        template<typename T>
        struct IsModifier : std::false_type {};
        
        template<typename T>
        struct IsModifier<Optional<T>> : std::true_type {};
        
        template<typename T>
        struct IsModifier<Not<T>> : std::true_type {};
        
        template<typename... Ts>
        struct IsModifier<Any<Ts...>> : std::true_type {};
        
        template<typename... Ts>
        struct IsModifier<OneOf<Ts...>> : std::true_type {};
        
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
        
        // GCC fix: Move GetRequiredImpl to namespace scope to allow template specializations
        // Helper template to extract required components
        template<typename... Args>
        struct GetRequiredImpl;
        
        // Base case: no arguments
        template<>
        struct GetRequiredImpl<>
        {
            using type = std::tuple<>;
        };
        
        // Recursive case: process one argument at a time
        template<typename T, typename... Rest>
        struct GetRequiredImpl<T, Rest...>
        {
            using type = std::conditional_t<
                IsModifier_v<T>,
                typename GetRequiredImpl<Rest...>::type,
                decltype(std::tuple_cat(std::tuple<T>{}, std::declval<typename GetRequiredImpl<Rest...>::type>()))
            >;
        };
        
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
            
            // GCC fix: Use GetRequiredImpl moved to namespace scope
            template<typename... Args>
            struct GetRequired
            {
                using type = typename GetRequiredImpl<Args...>::type;
            };
            
            using RequiredComponents = typename GetRequired<QueryArgs...>::type;
            using OptionalComponents = typename FilterByModifier<Optional, QueryArgs...>::type;
            using ExcludedComponents = typename FilterByModifier<Not, QueryArgs...>::type;
            using AnyGroups = typename FilterByModifier<Any, QueryArgs...>::type;
            using OneOfGroups = typename FilterByModifier<OneOf, QueryArgs...>::type;
        };
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
            // Must have all required components
            if (!archetypeMask.HasAll(GetRequiredMask()))
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
