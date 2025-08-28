#pragma once

#include <type_traits>
#include <utility>

#include "../Container/AlignedStorage.hpp"
#include "Base.hpp"

namespace Astra
{

    template<typename T, typename E>
    class Result
    {
    public:
        using ValueType = T;
        using ErrorType = E;

        Result(const Result& other) : m_hasValue(other.m_hasValue)
        {
            if (m_hasValue)
            {
                new (m_storage.As<T>()) T(*other.m_storage.As<T>());
            }
            else
            {
                new (m_storage.As<E>()) E(*other.m_storage.As<E>());
            }
        }

        Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>) : m_hasValue(other.m_hasValue)
        {
            if (m_hasValue)
            {
                new (m_storage.As<T>()) T(std::move(*other.m_storage.As<T>()));
            }
            else
            {
                new (m_storage.As<E>()) E(std::move(*other.m_storage.As<E>()));
            }
        }

        ~Result()
        {
            if (m_hasValue)
            {
                m_storage.As<T>()->~T();
            }
            else
            {
                m_storage.As<E>()->~E();
            }
        }

        Result& operator=(const Result& other)
        {
            if (this != &other)
            {
                if (m_hasValue && other.m_hasValue)
                {
                    *m_storage.As<T>() = *other.m_storage.As<T>();
                }
                else if (!m_hasValue && !other.m_hasValue)
                {
                    *m_storage.As<E>() = *other.m_storage.As<E>();
                }
                else
                {
                    this->~Result();
                    new (this) Result(other);
                }
            }
            return *this;
        }

        Result& operator=(Result&& other) noexcept(std::is_nothrow_move_assignable_v<T> && std::is_nothrow_move_assignable_v<E>
                                                   && std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>)
        {
            if (this != &other)
            {
                if (m_hasValue && other.m_hasValue)
                {
                    *m_storage.As<T>() = std::move(*other.m_storage.As<T>());
                }
                else if (!m_hasValue && !other.m_hasValue)
                {
                    *m_storage.As<E>() = std::move(*other.m_storage.As<E>());
                }
                else
                {
                    this->~Result();
                    new (this) Result(std::move(other));
                }
            }
            return *this;
        }

        template<typename U = T>
        static Result Ok(U&& value)
        {
            return Result(std::in_place_index<0>, std::forward<U>(value));
        }

        template<typename U = E>
        static Result Err(U&& error)
        {
            return Result(std::in_place_index<1>, std::forward<U>(error));
        }

        ASTRA_NODISCARD constexpr bool IsOk() const noexcept { return m_hasValue; }
        ASTRA_NODISCARD constexpr bool IsErr() const noexcept { return !m_hasValue; }

        ASTRA_NODISCARD T* GetValue() noexcept
        {
            return m_hasValue ? m_storage.As<T>() : nullptr;
        }

        ASTRA_NODISCARD const T* GetValue() const noexcept
        {
            return m_hasValue ? m_storage.As<T>() : nullptr;
        }

        ASTRA_NODISCARD E* GetError() noexcept
        {
            return !m_hasValue ? m_storage.As<E>() : nullptr;
        }

        ASTRA_NODISCARD const E* GetError() const noexcept
        {
            return !m_hasValue ? m_storage.As<E>() : nullptr;
        }

        ASTRA_NODISCARD T* operator->()
        {
            ASTRA_ASSERT(m_hasValue, "Dereferencing Result with no value");
            return m_storage.As<T>();
        }

        ASTRA_NODISCARD const T* operator->() const
        {
            ASTRA_ASSERT(m_hasValue, "Dereferencing Result with no value");
            return m_storage.As<T>();
        }

        ASTRA_NODISCARD T& operator*() & noexcept
        {
            return *m_storage.As<T>();
        }

        ASTRA_NODISCARD const T& operator*() const& noexcept
        {
            return *m_storage.As<T>();
        }

        ASTRA_NODISCARD T&& operator*() && noexcept
        {
            return std::move(*m_storage.As<T>());
        }

        template<typename F>
        ASTRA_NODISCARD auto Map(F&& func) -> Result<decltype(func(std::declval<T>())), E>
        {
            using NewT = decltype(func(std::declval<T>()));
            if (m_hasValue)
            {
                return Result<NewT, E>::Ok(func(*m_storage.As<T>()));
            }
            else
            {
                return Result<NewT, E>::Err(*m_storage.As<E>());
            }
        }

        template<typename F>
        ASTRA_NODISCARD auto MapError(F&& func) -> Result<T, decltype(func(std::declval<E>()))>
        {
            using NewE = decltype(func(std::declval<E>()));
            if (m_hasValue)
            {
                return Result<T, NewE>::Ok(*m_storage.As<T>());
            }
            else
            {
                return Result<T, NewE>::Err(func(*m_storage.As<E>()));
            }
        }

        template<typename F>
        ASTRA_NODISCARD auto AndThen(F&& func) -> decltype(func(std::declval<T>()))
        {
            using ReturnType = decltype(func(std::declval<T>()));
            static_assert(std::is_same_v<typename ReturnType::ErrorType, E>, "AndThen function must return Result with same error type");
            
            if (m_hasValue)
            {
                return func(*m_storage.As<T>());
            }
            else
            {
                return ReturnType::Err(*m_storage.As<E>());
            }
        }

        template<typename U>
        ASTRA_NODISCARD T ValueOr(U&& defaultValue) const&
        {
            return m_hasValue ? *m_storage.As<T>() : static_cast<T>(std::forward<U>(defaultValue));
        }

        template<typename U>
        ASTRA_NODISCARD T ValueOr(U&& defaultValue) &&
        {
            return m_hasValue ? std::move(*m_storage.As<T>()) : static_cast<T>(std::forward<U>(defaultValue));
        }

    private:
        template<typename U>
        explicit Result(std::in_place_index_t<0>, U&& value) : m_hasValue(true)
        {
            new (m_storage.As<T>()) T(std::forward<U>(value));
        }

        template<typename U>
        explicit Result(std::in_place_index_t<1>, U&& error) : m_hasValue(false)
        {
            new (m_storage.As<E>()) E(std::forward<U>(error));
        }

        AlignedStorage<T, E> m_storage;
        bool m_hasValue;
    };

    template<typename E>
    class Result<void, E>
    {
    public:
        using ValueType = void;
        using ErrorType = E;

        Result(const Result& other) : m_hasValue(other.m_hasValue)
        {
            if (!m_hasValue)
            {
                new (m_storage.As<E>()) E(*other.m_storage.As<E>());
            }
        }

        Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<E>) : m_hasValue(other.m_hasValue)
        {
            if (!m_hasValue)
            {
                new (m_storage.As<E>()) E(std::move(*other.m_storage.As<E>()));
            }
        }

        ~Result()
        {
            if (!m_hasValue)
            {
                m_storage.As<E>()->~E();
            }
        }

        Result& operator=(const Result& other)
        {
            if (this != &other)
            {
                if (!m_hasValue && !other.m_hasValue)
                {
                    *m_storage.As<E>() = *other.m_storage.As<E>();
                }
                else if (m_hasValue != other.m_hasValue)
                {
                    this->~Result();
                    new (this) Result(other);
                }
            }
            return *this;
        }

        Result& operator=(Result&& other) noexcept(std::is_nothrow_move_assignable_v<E> && std::is_nothrow_move_constructible_v<E>)
        {
            if (this != &other)
            {
                if (!m_hasValue && !other.m_hasValue)
                {
                    *m_storage.As<E>() = std::move(*other.m_storage.As<E>());
                }
                else if (m_hasValue != other.m_hasValue)
                {
                    this->~Result();
                    new (this) Result(std::move(other));
                }
            }
            return *this;
        }

        static Result Ok()
        {
            return Result(std::in_place_index<0>);
        }

        template<typename U = E>
        static Result Err(U&& error)
        {
            return Result(std::in_place_index<1>, std::forward<U>(error));
        }

        ASTRA_NODISCARD constexpr bool IsOk() const noexcept { return m_hasValue; }
        ASTRA_NODISCARD constexpr bool IsErr() const noexcept { return !m_hasValue; }

        ASTRA_NODISCARD E* GetError() noexcept
        {
            return !m_hasValue ? m_storage.As<E>() : nullptr;
        }

        ASTRA_NODISCARD const E* GetError() const noexcept
        {
            return !m_hasValue ? m_storage.As<E>() : nullptr;
        }

        template<typename F>
        ASTRA_NODISCARD auto MapError(F&& func) -> Result<void, decltype(func(std::declval<E>()))>
        {
            using NewE = decltype(func(std::declval<E>()));
            if (m_hasValue)
            {
                return Result<void, NewE>::Ok();
            }
            else
            {
                return Result<void, NewE>::Err(func(*m_storage.As<E>()));
            }
        }

        template<typename F>
        ASTRA_NODISCARD auto AndThen(F&& func) -> decltype(func())
        {
            using ReturnType = decltype(func());
            static_assert(std::is_same_v<typename ReturnType::ErrorType, E>, "AndThen function must return Result with same error type");
            
            if (m_hasValue)
            {
                return func();
            }
            else
            {
                return ReturnType::Err(*m_storage.As<E>());
            }
        }

    private:
        explicit Result(std::in_place_index_t<0>) : m_hasValue(true) {}

        template<typename U>
        explicit Result(std::in_place_index_t<1>, U&& error) : m_hasValue(false)
        {
            new (m_storage.As<E>()) E(std::forward<U>(error));
        }

        AlignedStorage<void, E> m_storage;
        bool m_hasValue;
    };
}