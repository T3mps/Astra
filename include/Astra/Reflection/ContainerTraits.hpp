#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <forward_list>
#include <list>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Astra
{
    // ============================================================================
    // Primary template for container traits - default is not a container
    // ============================================================================

    template<typename T, typename = void>
    struct ContainerTraits
    {
        static constexpr bool IsContainer = false;
        static constexpr bool IsSequence = false;
        static constexpr bool IsAssociative = false;
        static constexpr bool IsSet = false;
        static constexpr bool IsMap = false;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = false;
        static constexpr size_t FixedSize = 0;

        using ValueType = void;
        using KeyType = void;
        using MappedType = void;
    };

    // ============================================================================
    // Helper to detect if a type has begin/end (iterable)
    // ============================================================================

    namespace Detail
    {
        template<typename T, typename = void>
        struct HasIterators : std::false_type {};

        template<typename T>
        struct HasIterators<T, std::void_t<
            decltype(std::declval<T>().begin()),
            decltype(std::declval<T>().end())
        >> : std::true_type {};

        template<typename T, typename = void>
        struct HasSize : std::false_type {};

        template<typename T>
        struct HasSize<T, std::void_t<decltype(std::declval<T>().size())>>
            : std::true_type {};

        template<typename T, typename = void>
        struct HasPushBack : std::false_type {};

        template<typename T>
        struct HasPushBack<T, std::void_t<
            decltype(std::declval<T>().push_back(std::declval<typename T::value_type>()))
        >> : std::true_type {};

        template<typename T, typename = void>
        struct HasInsert : std::false_type {};

        template<typename T>
        struct HasInsert<T, std::void_t<
            decltype(std::declval<T>().insert(std::declval<typename T::value_type>()))
        >> : std::true_type {};
    }

    // ============================================================================
    // std::vector specialization
    // ============================================================================

    template<typename T, typename Alloc>
    struct ContainerTraits<std::vector<T, Alloc>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = true;
        static constexpr bool IsAssociative = false;
        static constexpr bool IsSet = false;
        static constexpr bool IsMap = false;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = true;
        static constexpr size_t FixedSize = 0;

        using ValueType = T;
        using KeyType = void;
        using MappedType = void;

        static size_t Size(const std::vector<T, Alloc>& c) { return c.size(); }
        static void Clear(std::vector<T, Alloc>& c) { c.clear(); }
        static void Reserve(std::vector<T, Alloc>& c, size_t n) { c.reserve(n); }
        static void PushBack(std::vector<T, Alloc>& c, const T& v) { c.push_back(v); }
        static void PushBack(std::vector<T, Alloc>& c, T&& v) { c.push_back(std::move(v)); }
        static T* Data(std::vector<T, Alloc>& c) { return c.data(); }
        static const T* Data(const std::vector<T, Alloc>& c) { return c.data(); }
    };

    // ============================================================================
    // std::array specialization
    // ============================================================================

    template<typename T, size_t N>
    struct ContainerTraits<std::array<T, N>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = true;
        static constexpr bool IsAssociative = false;
        static constexpr bool IsSet = false;
        static constexpr bool IsMap = false;
        static constexpr bool HasFixedSize = true;
        static constexpr bool HasContiguousStorage = true;
        static constexpr size_t FixedSize = N;

        using ValueType = T;
        using KeyType = void;
        using MappedType = void;

        static constexpr size_t Size(const std::array<T, N>&) { return N; }
        static T* Data(std::array<T, N>& c) { return c.data(); }
        static const T* Data(const std::array<T, N>& c) { return c.data(); }
    };

    // ============================================================================
    // std::deque specialization
    // ============================================================================

    template<typename T, typename Alloc>
    struct ContainerTraits<std::deque<T, Alloc>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = true;
        static constexpr bool IsAssociative = false;
        static constexpr bool IsSet = false;
        static constexpr bool IsMap = false;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = false;
        static constexpr size_t FixedSize = 0;

        using ValueType = T;
        using KeyType = void;
        using MappedType = void;

        static size_t Size(const std::deque<T, Alloc>& c) { return c.size(); }
        static void Clear(std::deque<T, Alloc>& c) { c.clear(); }
        static void PushBack(std::deque<T, Alloc>& c, const T& v) { c.push_back(v); }
        static void PushBack(std::deque<T, Alloc>& c, T&& v) { c.push_back(std::move(v)); }
    };

    // ============================================================================
    // std::list specialization
    // ============================================================================

    template<typename T, typename Alloc>
    struct ContainerTraits<std::list<T, Alloc>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = true;
        static constexpr bool IsAssociative = false;
        static constexpr bool IsSet = false;
        static constexpr bool IsMap = false;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = false;
        static constexpr size_t FixedSize = 0;

        using ValueType = T;
        using KeyType = void;
        using MappedType = void;

        static size_t Size(const std::list<T, Alloc>& c) { return c.size(); }
        static void Clear(std::list<T, Alloc>& c) { c.clear(); }
        static void PushBack(std::list<T, Alloc>& c, const T& v) { c.push_back(v); }
        static void PushBack(std::list<T, Alloc>& c, T&& v) { c.push_back(std::move(v)); }
    };

    // ============================================================================
    // std::forward_list specialization
    // ============================================================================

    template<typename T, typename Alloc>
    struct ContainerTraits<std::forward_list<T, Alloc>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = true;
        static constexpr bool IsAssociative = false;
        static constexpr bool IsSet = false;
        static constexpr bool IsMap = false;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = false;
        static constexpr size_t FixedSize = 0;

        using ValueType = T;
        using KeyType = void;
        using MappedType = void;

        static void Clear(std::forward_list<T, Alloc>& c) { c.clear(); }
        static void PushFront(std::forward_list<T, Alloc>& c, const T& v) { c.push_front(v); }
        static void PushFront(std::forward_list<T, Alloc>& c, T&& v) { c.push_front(std::move(v)); }
    };

    // ============================================================================
    // std::set specialization
    // ============================================================================

    template<typename T, typename Compare, typename Alloc>
    struct ContainerTraits<std::set<T, Compare, Alloc>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = false;
        static constexpr bool IsAssociative = true;
        static constexpr bool IsSet = true;
        static constexpr bool IsMap = false;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = false;
        static constexpr size_t FixedSize = 0;

        using ValueType = T;
        using KeyType = T;
        using MappedType = void;

        static size_t Size(const std::set<T, Compare, Alloc>& c) { return c.size(); }
        static void Clear(std::set<T, Compare, Alloc>& c) { c.clear(); }
        static void Insert(std::set<T, Compare, Alloc>& c, const T& v) { c.insert(v); }
        static void Insert(std::set<T, Compare, Alloc>& c, T&& v) { c.insert(std::move(v)); }
    };

    // ============================================================================
    // std::unordered_set specialization
    // ============================================================================

    template<typename T, typename Hash, typename Eq, typename Alloc>
    struct ContainerTraits<std::unordered_set<T, Hash, Eq, Alloc>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = false;
        static constexpr bool IsAssociative = true;
        static constexpr bool IsSet = true;
        static constexpr bool IsMap = false;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = false;
        static constexpr size_t FixedSize = 0;

        using ValueType = T;
        using KeyType = T;
        using MappedType = void;

        static size_t Size(const std::unordered_set<T, Hash, Eq, Alloc>& c) { return c.size(); }
        static void Clear(std::unordered_set<T, Hash, Eq, Alloc>& c) { c.clear(); }
        static void Insert(std::unordered_set<T, Hash, Eq, Alloc>& c, const T& v) { c.insert(v); }
        static void Insert(std::unordered_set<T, Hash, Eq, Alloc>& c, T&& v) { c.insert(std::move(v)); }
    };

    // ============================================================================
    // std::map specialization
    // ============================================================================

    template<typename K, typename V, typename Compare, typename Alloc>
    struct ContainerTraits<std::map<K, V, Compare, Alloc>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = false;
        static constexpr bool IsAssociative = true;
        static constexpr bool IsSet = false;
        static constexpr bool IsMap = true;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = false;
        static constexpr size_t FixedSize = 0;

        using ValueType = std::pair<const K, V>;
        using KeyType = K;
        using MappedType = V;

        static size_t Size(const std::map<K, V, Compare, Alloc>& c) { return c.size(); }
        static void Clear(std::map<K, V, Compare, Alloc>& c) { c.clear(); }
        static void Insert(std::map<K, V, Compare, Alloc>& c, const K& k, const V& v) { c[k] = v; }
        static void Insert(std::map<K, V, Compare, Alloc>& c, K&& k, V&& v) { c[std::move(k)] = std::move(v); }
    };

    // ============================================================================
    // std::unordered_map specialization
    // ============================================================================

    template<typename K, typename V, typename Hash, typename Eq, typename Alloc>
    struct ContainerTraits<std::unordered_map<K, V, Hash, Eq, Alloc>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = false;
        static constexpr bool IsAssociative = true;
        static constexpr bool IsSet = false;
        static constexpr bool IsMap = true;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = false;
        static constexpr size_t FixedSize = 0;

        using ValueType = std::pair<const K, V>;
        using KeyType = K;
        using MappedType = V;

        static size_t Size(const std::unordered_map<K, V, Hash, Eq, Alloc>& c) { return c.size(); }
        static void Clear(std::unordered_map<K, V, Hash, Eq, Alloc>& c) { c.clear(); }
        static void Insert(std::unordered_map<K, V, Hash, Eq, Alloc>& c, const K& k, const V& v) { c[k] = v; }
        static void Insert(std::unordered_map<K, V, Hash, Eq, Alloc>& c, K&& k, V&& v) { c[std::move(k)] = std::move(v); }
    };

    // ============================================================================
    // std::string specialization
    // ============================================================================

    template<typename CharT, typename Traits, typename Alloc>
    struct ContainerTraits<std::basic_string<CharT, Traits, Alloc>>
    {
        static constexpr bool IsContainer = true;
        static constexpr bool IsSequence = true;
        static constexpr bool IsAssociative = false;
        static constexpr bool IsSet = false;
        static constexpr bool IsMap = false;
        static constexpr bool HasFixedSize = false;
        static constexpr bool HasContiguousStorage = true;
        static constexpr bool IsString = true;
        static constexpr size_t FixedSize = 0;

        using ValueType = CharT;
        using KeyType = void;
        using MappedType = void;

        static size_t Size(const std::basic_string<CharT, Traits, Alloc>& c) { return c.size(); }
        static void Clear(std::basic_string<CharT, Traits, Alloc>& c) { c.clear(); }
        static void Reserve(std::basic_string<CharT, Traits, Alloc>& c, size_t n) { c.reserve(n); }
        static CharT* Data(std::basic_string<CharT, Traits, Alloc>& c) { return c.data(); }
        static const CharT* Data(const std::basic_string<CharT, Traits, Alloc>& c) { return c.data(); }
    };

    // ============================================================================
    // Convenience type traits
    // ============================================================================

    template<typename T>
    inline constexpr bool IsContainer_v = ContainerTraits<T>::IsContainer;

    template<typename T>
    inline constexpr bool IsSequenceContainer_v = ContainerTraits<T>::IsSequence;

    template<typename T>
    inline constexpr bool IsAssociativeContainer_v = ContainerTraits<T>::IsAssociative;

    template<typename T>
    inline constexpr bool IsSetContainer_v = ContainerTraits<T>::IsSet;

    template<typename T>
    inline constexpr bool IsMapContainer_v = ContainerTraits<T>::IsMap;

    template<typename T>
    inline constexpr bool HasFixedSize_v = ContainerTraits<T>::HasFixedSize;

    template<typename T>
    inline constexpr bool HasContiguousStorage_v = ContainerTraits<T>::HasContiguousStorage;

    // ============================================================================
    // C-style array detection
    // ============================================================================

    template<typename T>
    struct ArrayTraits
    {
        static constexpr bool IsArray = false;
        static constexpr size_t Size = 0;
        using ElementType = void;
    };

    template<typename T, size_t N>
    struct ArrayTraits<T[N]>
    {
        static constexpr bool IsArray = true;
        static constexpr size_t Size = N;
        using ElementType = T;
    };

    template<typename T>
    inline constexpr bool IsArray_v = ArrayTraits<T>::IsArray;

    template<typename T>
    inline constexpr size_t ArraySize_v = ArrayTraits<T>::Size;
}
