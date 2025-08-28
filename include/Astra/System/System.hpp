#pragma once

#include <concepts>
#include <tuple>
#include <type_traits>

#include "../Component/Component.hpp"
#include "../Core/Base.hpp"
#include "../Registry/Registry.hpp"

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
    
    template<typename... Traits>
    struct SystemTraits {};
    
    template<typename... ReadComponents, typename... WriteComponents>
    struct SystemTraits<Reads<ReadComponents...>, Writes<WriteComponents...>>
    {
        using ReadsComponents = std::tuple<ReadComponents...>;
        using WritesComponents = std::tuple<WriteComponents...>;
        static constexpr bool HasTraits = true;
    };
    
    template<typename... ReadComponents>
    struct SystemTraits<Reads<ReadComponents...>>
    {
        using ReadsComponents = std::tuple<ReadComponents...>;
        using WritesComponents = std::tuple<>;
        static constexpr bool HasTraits = true;
    };
    
    template<typename... WriteComponents>
    struct SystemTraits<Writes<WriteComponents...>>
    {
        using ReadsComponents = std::tuple<>;
        using WritesComponents = std::tuple<WriteComponents...>;
        static constexpr bool HasTraits = true;
    };
    
    template<typename T>
    struct HasSystemTraits : std::false_type {};
    
    template<typename T>
    requires requires { typename T::ReadsComponents; typename T::WritesComponents; T::HasTraits; }
    struct HasSystemTraits<T> : std::true_type {};
    
    template<typename T>
    inline constexpr bool HasSystemTraits_v = HasSystemTraits<T>::value;
    
    template<typename T>
    concept LambdaLike = requires
    {
        &T::operator();  // Has operator()
    } && !std::invocable<T, Registry&>;  // But not a traditional system

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
        using ReadsComponents = decltype(ExtractReads<ComponentArgs>(std::make_index_sequence<std::tuple_size_v<ComponentArgs>>{}));
        using WritesComponents = decltype(ExtractWrites<ComponentArgs>(std::make_index_sequence<std::tuple_size_v<ComponentArgs>>{}));
        static constexpr bool HasTraits = true;
        
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

            // Create view preserving const-ness of components
            // Convert const T& to const T, and T& to T
            auto view = registry.CreateView<
                std::conditional_t<IsReadOnly<Components>,
                    const BaseType<Components>,
                        BaseType<Components>
                >...
            >();
            view.ForEach(m_lambda);
        }

        Lambda m_lambda;
    };
}
