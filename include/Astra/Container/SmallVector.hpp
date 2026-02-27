#pragma once

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

#include "../Core/Base.hpp"

namespace Astra
{
    /**
     * @brief A vector-like container with small buffer optimization
     * 
     * SmallVector stores up to N elements in an internal buffer without heap allocation.
     * When the size exceeds N, it dynamically allocates memory like std::vector.
     * 
     * This is ideal for cases where you typically have few elements (e.g., entity relationships)
     * but occasionally need to handle more.
     * 
     * @tparam T Element type
     * @tparam N Number of elements to store inline (small buffer size)
     */
    template<typename T, size_t N = 4>
    class SmallVector
    {
        static_assert(N > 0, "SmallVector must have at least 1 inline element");
        
    public:
        using value_type = T;
        using size_type = size_t;
        using difference_type = ptrdiff_t;
        using reference = T&;
        using const_reference = const T&;
        using pointer = T*;
        using const_pointer = const T*;
        using iterator = T*;
        using const_iterator = const T*;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;
        
        SmallVector() noexcept 
            : m_data(reinterpret_cast<T*>(m_buffer))
            , m_size(0)
            , m_capacity(N) 
        {
        }
        
        explicit SmallVector(size_type count)
            : m_data(reinterpret_cast<T*>(m_buffer))
            , m_size(0)
            , m_capacity(N)
        {
            resize(count);
        }
        
        SmallVector(size_type count, const T& value)
            : m_data(reinterpret_cast<T*>(m_buffer))
            , m_size(0)
            , m_capacity(N)
        {
            resize(count, value);
        }
        
        template<typename InputIt, typename = std::enable_if_t<!std::is_integral_v<InputIt>>>
        SmallVector(InputIt first, InputIt last)
            : m_data(reinterpret_cast<T*>(m_buffer))
            , m_size(0)
            , m_capacity(N)
        {
            assign(first, last);
        }
        
        SmallVector(std::initializer_list<T> init)
            : m_data(reinterpret_cast<T*>(m_buffer))
            , m_size(0)
            , m_capacity(N)
        {
            assign(init);
        }
        
        SmallVector(const SmallVector& other)
            : m_data(reinterpret_cast<T*>(m_buffer))
            , m_size(0)
            , m_capacity(N)
        {
            assign(other.begin(), other.end());
        }
        
        SmallVector(SmallVector&& other) noexcept
            : m_data(reinterpret_cast<T*>(m_buffer))
            , m_size(0)
            , m_capacity(N)
        {
            if (other.IsSmall())
            {
                // Move elements from small buffer
                std::uninitialized_move(other.begin(), other.end(), begin());
                m_size = other.m_size;
                other.clear();
            }
            else
            {
                // Steal heap allocation
                m_data = other.m_data;
                m_capacity = other.m_capacity;
                m_size = other.m_size;
                
                // Reset other to small buffer
                other.m_data = other.GetBuffer();
                other.m_capacity = N;
                other.m_size = 0;
            }
        }
        
        ~SmallVector()
        {
            clear();
            if (!IsSmall())
            {
                ::operator delete(m_data);
            }
        }
        
        SmallVector& operator=(const SmallVector& other)
        {
            if (this != &other)
            {
                assign(other.begin(), other.end());
            }
            return *this;
        }
        
        SmallVector& operator=(SmallVector&& other) noexcept
        {
            if (this != &other)
            {
                // Clean up current contents
                clear();
                if (!IsSmall())
                {
                    ::operator delete(m_data);
                }

                if (other.IsSmall())
                {
                    // Move elements from small buffer
                    m_data = reinterpret_cast<T*>(m_buffer);
                    m_capacity = N;
                    std::uninitialized_move(other.begin(), other.end(), begin());
                    m_size = other.m_size;
                    other.clear();
                }
                else
                {
                    // Steal heap allocation
                    m_data = other.m_data;
                    m_capacity = other.m_capacity;
                    m_size = other.m_size;

                    // Reset other to small buffer
                    other.m_data = other.GetBuffer();
                    other.m_capacity = N;
                    other.m_size = 0;
                }
            }
            return *this;
        }
        
        SmallVector& operator=(std::initializer_list<T> init)
        {
            assign(init);
            return *this;
        }
        
        void assign(size_type count, const T& value)
        {
            clear();
            resize(count, value);
        }
        
        template<typename InputIt, typename = std::enable_if_t<!std::is_integral_v<InputIt>>>
        void assign(InputIt first, InputIt last)
        {
            clear();
            for (; first != last; ++first)
            {
                push_back(*first);
            }
        }
        
        void assign(std::initializer_list<T> init)
        {
            assign(init.begin(), init.end());
        }
        
        [[nodiscard]] reference at(size_type pos)
        {
            ASTRA_ASSERT(pos < m_size, "SmallVector::at() out of range");
            return data()[pos];
        }
        
        [[nodiscard]] const_reference at(size_type pos) const
        {
            ASTRA_ASSERT(pos < m_size, "SmallVector::at() out of range");
            return data()[pos];
        }
        
        [[nodiscard]] reference operator[](size_type pos) noexcept
        {
            ASTRA_ASSERT(pos < m_size, "SmallVector::operator[] out of range");
            return data()[pos];
        }
        
        [[nodiscard]] const_reference operator[](size_type pos) const noexcept
        {
            ASTRA_ASSERT(pos < m_size, "SmallVector::operator[] out of range");
            return data()[pos];
        }
        
        [[nodiscard]] reference front() noexcept
        {
            ASTRA_ASSERT(!empty(), "SmallVector::front() on empty vector");
            return data()[0];
        }
        
        [[nodiscard]] const_reference front() const noexcept
        {
            ASTRA_ASSERT(!empty(), "SmallVector::front() on empty vector");
            return data()[0];
        }
        
        [[nodiscard]] reference back() noexcept
        {
            ASTRA_ASSERT(!empty(), "SmallVector::back() on empty vector");
            return data()[m_size - 1];
        }
        
        [[nodiscard]] const_reference back() const noexcept
        {
            ASTRA_ASSERT(!empty(), "SmallVector::back() on empty vector");
            return data()[m_size - 1];
        }
        
        [[nodiscard]] T* data() noexcept
        {
            return m_data;
        }
        
        [[nodiscard]] const T* data() const noexcept
        {
            return m_data;
        }
        
        [[nodiscard]] iterator begin() noexcept { return data(); }
        [[nodiscard]] const_iterator begin() const noexcept { return data(); }
        [[nodiscard]] const_iterator cbegin() const noexcept { return data(); }
        
        [[nodiscard]] iterator end() noexcept { return data() + m_size; }
        [[nodiscard]] const_iterator end() const noexcept { return data() + m_size; }
        [[nodiscard]] const_iterator cend() const noexcept { return data() + m_size; }
        
        [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
        [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
        [[nodiscard]] const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(end()); }
        
        [[nodiscard]] reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
        [[nodiscard]] const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
        [[nodiscard]] const_reverse_iterator crend() const noexcept { return const_reverse_iterator(begin()); }
        
        [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
        [[nodiscard]] size_type size() const noexcept { return m_size; }
        [[nodiscard]] size_type max_size() const noexcept { return std::numeric_limits<size_type>::max(); }
        
        [[nodiscard]] size_type capacity() const noexcept
        {
            return m_capacity;
        }
        
        void reserve(size_type newCap)
        {
            if (newCap <= capacity())
                return;
                
            Grow(newCap);
        }
        
        void shrink_to_fit()
        {
            if (IsSmall() || m_size == capacity())
                return;
                
            if (m_size <= N)
            {
                // Move back to small buffer
                T* heapData = m_data;
                std::uninitialized_move(heapData, heapData + m_size, GetBuffer());
                std::destroy(heapData, heapData + m_size);
                ::operator delete(heapData);
                
                m_data = GetBuffer();
                m_capacity = N;
            }
            else if (m_size < capacity())
            {
                // Reallocate to exact size
                T* newData = static_cast<T*>(::operator new(m_size * sizeof(T)));
                std::uninitialized_move(begin(), end(), newData);
                std::destroy(begin(), end());
                ::operator delete(m_data);
                
                m_data = newData;
                m_capacity = m_size;
            }
        }
        
        void clear() noexcept
        {
            std::destroy(begin(), end());
            m_size = 0;
        }
        
        iterator insert(const_iterator pos, const T& value)
        {
            return emplace(pos, value);
        }
        
        iterator insert(const_iterator pos, T&& value)
        {
            return emplace(pos, std::move(value));
        }
        
        iterator insert(const_iterator pos, size_type count, const T& value)
        {
            size_type offset = pos - cbegin();
            
            if (count == 0)
                return begin() + offset;
                
            if (m_size + count > capacity())
                Grow(m_size + count);
                
            iterator it = begin() + offset;
            
            // Move existing elements
            if (it != end())
            {
                std::move_backward(it, end(), end() + count);
            }
            
            // Insert new elements
            std::uninitialized_fill_n(it, count, value);
            m_size += count;
            
            return it;
        }
        
        template<typename... Args>
        iterator emplace(const_iterator pos, Args&&... args)
        {
            size_type offset = pos - cbegin();
            
            if (m_size == capacity())
                Grow(m_size + 1);
                
            iterator it = begin() + offset;
            
            if (it == end())
            {
                // Construct at end
                std::construct_at(end(), std::forward<Args>(args)...);
            }
            else
            {
                // Move last element
                std::construct_at(end(), std::move(back()));
                
                // Move elements
                std::move_backward(it, end() - 1, end());
                
                // Destroy and reconstruct
                std::destroy_at(it);
                std::construct_at(it, std::forward<Args>(args)...);
            }
            
            ++m_size;
            return it;
        }
        
        iterator erase(const_iterator pos)
        {
            return erase(pos, pos + 1);
        }
        
        iterator erase(const_iterator first, const_iterator last)
        {
            size_type offset = first - cbegin();
            size_type count = last - first;
            
            if (count == 0)
                return begin() + offset;
                
            iterator it = begin() + offset;
            
            // Move elements
            std::move(it + count, end(), it);
            
            // Destroy moved-from elements
            std::destroy(end() - count, end());
            
            m_size -= count;
            return it;
        }
        
        void push_back(const T& value)
        {
            emplace_back(value);
        }
        
        void push_back(T&& value)
        {
            emplace_back(std::move(value));
        }
        
        template<typename... Args>
        reference emplace_back(Args&&... args)
        {
            if (m_size == capacity())
                Grow(m_size + 1);
                
            std::construct_at(end(), std::forward<Args>(args)...);
            ++m_size;
            return back();
        }
        
        void pop_back() noexcept
        {
            ASTRA_ASSERT(!empty(), "SmallVector::pop_back() on empty vector");
            --m_size;
            std::destroy_at(end());
        }
        
        void resize(size_type count)
        {
            if (count < m_size)
            {
                std::destroy(begin() + count, end());
            }
            else if (count > m_size)
            {
                reserve(count);
                std::uninitialized_value_construct(end(), begin() + count);
            }
            m_size = count;
        }
        
        void resize(size_type count, const T& value)
        {
            if (count < m_size)
            {
                std::destroy(begin() + count, end());
            }
            else if (count > m_size)
            {
                reserve(count);
                std::uninitialized_fill(end(), begin() + count, value);
            }
            m_size = count;
        }
        
        void swap(SmallVector& other) noexcept
        {
            if (IsSmall() && other.IsSmall())
            {
                // Both small - swap elements
                size_type commonSize = std::min(m_size, other.m_size);
                std::swap_ranges(begin(), begin() + commonSize, other.begin());
                
                if (m_size > other.m_size)
                {
                    std::uninitialized_move(begin() + commonSize, end(), other.begin() + commonSize);
                    std::destroy(begin() + commonSize, end());
                }
                else if (other.m_size > m_size)
                {
                    std::uninitialized_move(other.begin() + commonSize, other.end(), begin() + commonSize);
                    std::destroy(other.begin() + commonSize, other.end());
                }
                std::swap(m_size, other.m_size);
            }
            else if (!IsSmall() && !other.IsSmall())
            {
                // Both heap - swap pointers
                std::swap(m_data, other.m_data);
                std::swap(m_capacity, other.m_capacity);
                std::swap(m_size, other.m_size);
            }
            else
            {
                // One small, one heap - full swap
                SmallVector temp(std::move(*this));
                *this = std::move(other);
                other = std::move(temp);
            }
        }
        
        friend bool operator==(const SmallVector& lhs, const SmallVector& rhs)
        {
            return lhs.size() == rhs.size() && 
                   std::equal(lhs.begin(), lhs.end(), rhs.begin());
        }
        
        friend bool operator!=(const SmallVector& lhs, const SmallVector& rhs)
        {
            return !(lhs == rhs);
        }
        
        friend bool operator<(const SmallVector& lhs, const SmallVector& rhs)
        {
            return std::lexicographical_compare(lhs.begin(), lhs.end(),
                                              rhs.begin(), rhs.end());
        }
        
        friend bool operator<=(const SmallVector& lhs, const SmallVector& rhs)
        {
            return !(rhs < lhs);
        }
        
        friend bool operator>(const SmallVector& lhs, const SmallVector& rhs)
        {
            return rhs < lhs;
        }
        
        friend bool operator>=(const SmallVector& lhs, const SmallVector& rhs)
        {
            return !(lhs < rhs);
        }
        
    private:
        // Get buffer pointer
        [[nodiscard]] T* GetBuffer() noexcept
        {
            return reinterpret_cast<T*>(m_buffer);
        }
        
        [[nodiscard]] const T* GetBuffer() const noexcept
        {
            return reinterpret_cast<const T*>(m_buffer);
        }
        
        // Check if using small buffer
        [[nodiscard]] bool IsSmall() const noexcept
        {
            return m_data == reinterpret_cast<const T*>(m_buffer);
        }
        
        // Grow capacity
        void Grow(size_type newCap)
        {
            newCap = std::max(newCap, std::max<size_type>(capacity() * 2, 4));
            
            T* newData = static_cast<T*>(::operator new(newCap * sizeof(T)));
            
            // Move existing elements
            std::uninitialized_move(begin(), end(), newData);
            
            // Destroy old elements
            std::destroy(begin(), end());
            
            // Free old memory if on heap
            if (!IsSmall())
            {
                ::operator delete(m_data);
            }
            
            // Update to new allocation
            m_data = newData;
            m_capacity = newCap;
        }
        
        // Member variables
        alignas(T) std::byte m_buffer[N * sizeof(T)];
        T* m_data;
        size_type m_size;
        size_type m_capacity;
    };
    
    // Deduction guide
    template<typename T, typename... U>
    SmallVector(T, U...) -> SmallVector<T, 1 + sizeof...(U)>;
}