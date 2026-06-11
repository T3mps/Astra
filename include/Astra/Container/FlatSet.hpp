#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
#include "../Core/Simd.hpp"
#include "../Entity/Entity.hpp"
#include "Swiss.hpp"

namespace Astra
{
    template<typename T,
             typename Hash = typename SelectHash<T>::Type,
             typename Equals = std::equal_to<T>,
             typename Allocator = std::allocator<T>>
    class FlatSet
    {
    public:
        using ValueType = T;
        using SizeType = std::size_t;
        using DifferenceType = std::ptrdiff_t;
        using Hasher = Hash;
        using KeyEqual = Equals;
        using AllocatorType = Allocator;
        using Reference = T&;
        using ConstReference = const T&;
        using Pointer = T*;
        using ConstPointer = const T*;
        
        class iterator
        {
            friend class FlatSet;
            friend class const_iterator;
            
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = T;
            using difference_type = DifferenceType;
            using pointer = Pointer;
            using reference = Reference;
            
            iterator() = default;
            
            iterator(FlatSet* set, SizeType index) noexcept
                : m_set(set), m_index(index)
            {
                SkipEmpty();
            }
            
            reference operator*() const noexcept
            {
                ASTRA_ASSERT(m_index < m_set->m_capacity, "Iterator out of bounds");
                ASTRA_ASSERT(m_set->m_slots != nullptr, "Dereferencing invalid iterator");
                
#ifdef ASTRA_BUILD_DEBUG
                SizeType groupIdx = m_index / GROUP_SIZE;
                SizeType slotIdx = m_index % GROUP_SIZE;
                std::uint8_t meta = m_set->m_groups[groupIdx].Get(slotIdx);
                ASTRA_ASSERT(meta != EMPTY && meta != TOMBSTONE, "Iterator points to invalid slot");
#endif
                
                return *m_set->m_slots[m_index].GetValue();
            }
            
            pointer operator->() const noexcept
            {
                ASTRA_ASSERT(m_index < m_set->m_capacity, "Iterator out of bounds");
                ASTRA_ASSERT(m_set->m_slots != nullptr, "Dereferencing invalid iterator");
                return m_set->m_slots[m_index].GetValue();
            }
            
            iterator& operator++() noexcept
            {
                ++m_index;
                SkipEmpty();
                return *this;
            }
            
            iterator operator++(int) noexcept
            {
                iterator tmp = *this;
                ++(*this);
                return tmp;
            }
            
            ASTRA_NODISCARD bool operator==(const iterator& other) const noexcept = default;
            
        private:
            FlatSet* m_set = nullptr;
            SizeType m_index = 0;
            
            void SkipEmpty() noexcept
            {
                if (!m_set) ASTRA_UNLIKELY return;
                
                while (m_index < m_set->m_capacity)
                {
                    SizeType groupIdx = m_index / GROUP_SIZE;
                    SizeType slotIdx = m_index % GROUP_SIZE;
                    
                    if (groupIdx < m_set->m_numGroups) ASTRA_LIKELY
                    {
                        std::uint8_t meta = m_set->m_groups[groupIdx].Get(slotIdx);
                        if (meta != EMPTY && meta != TOMBSTONE) ASTRA_LIKELY
                        {
                            return;
                        }
                    }
                    ++m_index;
                }
            }
        };
        
        class const_iterator
        {
            friend class FlatSet;
            
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = T;
            using difference_type = DifferenceType;
            using pointer = ConstPointer;
            using reference = ConstReference;
            
            const_iterator() = default;
            
            const_iterator(const FlatSet* set, SizeType index) noexcept
                : m_set(set), m_index(index)
            {
                SkipEmpty();
            }
            
            // Conversion from iterator to const_iterator
            const_iterator(const iterator& it) noexcept
                : m_set(it.m_set), m_index(it.m_index)
            {
            }
            
            reference operator*() const noexcept
            {
                ASTRA_ASSERT(m_index < m_set->m_capacity, "Iterator out of bounds");
                ASTRA_ASSERT(m_set->m_slots != nullptr, "Dereferencing invalid iterator");
                
#ifdef ASTRA_BUILD_DEBUG
                SizeType groupIdx = m_index / GROUP_SIZE;
                SizeType slotIdx = m_index % GROUP_SIZE;
                std::uint8_t meta = m_set->m_groups[groupIdx].Get(slotIdx);
                ASTRA_ASSERT(meta != EMPTY && meta != TOMBSTONE, "Iterator points to invalid slot");
#endif
                
                return *m_set->m_slots[m_index].GetValue();
            }
            
            pointer operator->() const noexcept
            {
                ASTRA_ASSERT(m_index < m_set->m_capacity, "Iterator out of bounds");
                ASTRA_ASSERT(m_set->m_slots != nullptr, "Dereferencing invalid iterator");
                return m_set->m_slots[m_index].GetValue();
            }
            
            const_iterator& operator++() noexcept
            {
                ++m_index;
                SkipEmpty();
                return *this;
            }
            
            const_iterator operator++(int) noexcept
            {
                const_iterator tmp = *this;
                ++(*this);
                return tmp;
            }
            
            ASTRA_NODISCARD bool operator==(const const_iterator& other) const noexcept = default;
            
        private:
            const FlatSet* m_set = nullptr;
            SizeType m_index = 0;
            
            void SkipEmpty() noexcept
            {
                if (!m_set) ASTRA_UNLIKELY return;
                
                while (m_index < m_set->m_capacity)
                {
                    SizeType groupIdx = m_index / GROUP_SIZE;
                    SizeType slotIdx = m_index % GROUP_SIZE;
                    
                    // Prefetch next group during iteration
                    if (slotIdx == 0 && groupIdx + 1 < m_set->m_numGroups) ASTRA_UNLIKELY
                    {
                        Simd::Ops::PrefetchT0(&m_set->m_groups[groupIdx + 1]);
                    }
                    
                    if (groupIdx < m_set->m_numGroups) ASTRA_LIKELY
                    {
                        std::uint8_t meta = m_set->m_groups[groupIdx].Get(slotIdx);
                        if (meta != EMPTY && meta != TOMBSTONE) ASTRA_LIKELY
                        {
                            return;
                        }
                    }
                    ++m_index;
                }
            }
        };
        
        FlatSet() = default;
        
        explicit FlatSet(SizeType capacity, const Hasher& hash = Hasher(), 
                        const KeyEqual& equal = KeyEqual(), const AllocatorType& alloc = AllocatorType())
            : m_hasher(hash)
            , m_equal(equal)
            , m_alloc(alloc)
            , m_slotAlloc(alloc)
            , m_groupAlloc(alloc)
        {
            if (capacity > 0) ASTRA_LIKELY
            {
                Reserve(capacity);
            }
        }
        
        ~FlatSet() noexcept
        {
            Clear();
            DeallocateStorage();
        }
        
        FlatSet(const FlatSet& other)
            : m_hasher(other.m_hasher)
            , m_equal(other.m_equal)
            , m_alloc(other.m_alloc)
            , m_slotAlloc(other.m_slotAlloc)
            , m_groupAlloc(other.m_groupAlloc)
        {
            if (other.m_size == 0) ASTRA_UNLIKELY return;
            
            const SizeType neededCapacity = [&]()
            {
                const SizeType minSize = static_cast<SizeType>(other.m_size / MAX_LOAD_FACTOR);
                if (minSize > std::numeric_limits<SizeType>::max() - GROUP_SIZE)
                {
                    return std::numeric_limits<SizeType>::max();
                }
                return minSize + GROUP_SIZE;
            }();
            Reserve(neededCapacity);
            
            // Copy elements group by group for cache efficiency
            for (SizeType groupIdx = 0; groupIdx < other.m_numGroups; ++groupIdx)
            {
                // Prefetch next group for better performance
                if (groupIdx + 1 < other.m_numGroups) ASTRA_LIKELY
                {
                    Simd::Ops::PrefetchT0(&other.m_groups[groupIdx + 1]);
                    Simd::Ops::PrefetchT0(&other.m_slots[(groupIdx + 1) * GROUP_SIZE]);
                }
                
                // Process all occupied slots in this group at once
                std::uint16_t occupied = other.m_groups[groupIdx].MatchOccupied();
                
                while (occupied)
                {
                    SizeType slotIdx = Simd::Ops::CountTrailingZeros(occupied);
                    SizeType srcIdx = groupIdx * GROUP_SIZE + slotIdx;
                    
                    if (srcIdx >= other.m_capacity) ASTRA_UNLIKELY
                    {
                        occupied &= occupied - 1;
                        continue;
                    }
                    
                    const T& value = *other.m_slots[srcIdx].GetValue();
                    Insert(value);
                    
                    // Clear this bit and continue
                    occupied &= occupied - 1;
                }
            }
            
            ASTRA_ASSERT(m_size == other.m_size, "Copy size mismatch");
        }
        
        FlatSet(FlatSet&& other) noexcept
            : m_groups(other.m_groups)
            , m_slots(other.m_slots)
            , m_capacity(other.m_capacity)
            , m_size(other.m_size)
            , m_numGroups(other.m_numGroups)
            , m_tombstoneCount(other.m_tombstoneCount)
            , m_hasher(std::move(other.m_hasher))
            , m_equal(std::move(other.m_equal))
            , m_alloc(std::move(other.m_alloc))
            , m_slotAlloc(std::move(other.m_slotAlloc))
            , m_groupAlloc(std::move(other.m_groupAlloc))
        {
            other.m_groups = nullptr;
            other.m_slots = nullptr;
            other.m_capacity = 0;
            other.m_size = 0;
            other.m_tombstoneCount = 0;
            other.m_numGroups = 0;
        }
        
        FlatSet& operator=(const FlatSet& other)
        {
            if (this != &other) ASTRA_LIKELY
            {
                FlatSet tmp(other);
                Swap(tmp);
            }
            return *this;
        }
        
        FlatSet& operator=(FlatSet&& other) noexcept
        {
            if (this != &other) ASTRA_LIKELY
            {
                Clear();
                DeallocateStorage();
                
                m_groups = other.m_groups;
                m_slots = other.m_slots;
                m_capacity = other.m_capacity;
                m_size = other.m_size;
                m_tombstoneCount = other.m_tombstoneCount;
                m_numGroups = other.m_numGroups;
                m_hasher = std::move(other.m_hasher);
                m_equal = std::move(other.m_equal);
                m_alloc = std::move(other.m_alloc);
                m_slotAlloc = std::move(other.m_slotAlloc);
                m_groupAlloc = std::move(other.m_groupAlloc);
                
                other.m_groups = nullptr;
                other.m_slots = nullptr;
                other.m_capacity = 0;
                other.m_size = 0;
                other.m_tombstoneCount = 0;
                other.m_numGroups = 0;
            }
            return *this;
        }
        
        ASTRA_NODISCARD iterator begin() noexcept { return iterator(this, 0); }
        ASTRA_NODISCARD const_iterator begin() const noexcept { return const_iterator(this, 0); }
        ASTRA_NODISCARD iterator end() noexcept { return iterator(this, m_capacity); }
        ASTRA_NODISCARD const_iterator end() const noexcept { return const_iterator(this, m_capacity); }
        
        ASTRA_NODISCARD bool Empty() const noexcept { return m_size == 0; }
        ASTRA_NODISCARD bool IsEmpty() const noexcept { return m_size == 0; }  // Alias for consistency
        ASTRA_NODISCARD SizeType Size() const noexcept { return m_size; }
        ASTRA_NODISCARD SizeType Capacity() const noexcept { return m_capacity; }
        
        void Clear() noexcept
        {
            if (!m_slots) ASTRA_UNLIKELY return;
            
            for (SizeType i = 0; i < m_capacity; ++i)
            {
                SizeType groupIdx = i / GROUP_SIZE;
                SizeType slotIdx = i % GROUP_SIZE;
                
                std::uint8_t meta = m_groups[groupIdx].Get(slotIdx);
                if (meta != EMPTY && meta != TOMBSTONE) ASTRA_UNLIKELY
                {
                    std::allocator_traits<AllocatorType>::destroy(m_alloc, m_slots[i].GetValue());
                    m_groups[groupIdx].Set(slotIdx, EMPTY);
                }
            }
            m_size = 0;
            m_tombstoneCount = 0;
        }
        
        template<typename K>
        iterator Find(const K& value) noexcept
        {
            std::uint8_t h2;
            SizeType idx = FindImpl(value, h2);
            return idx < m_capacity ? iterator(this, idx) : end();
        }
        
        template<typename K>
        const_iterator Find(const K& value) const noexcept
        {
            std::uint8_t h2;
            SizeType idx = FindImpl(value, h2);
            return idx < m_capacity ? const_iterator(this, idx) : end();
        }
        
        template<typename K>
        ASTRA_NODISCARD bool Contains(const K& value) const noexcept
        {
            return Find(value) != end();
        }
        
        std::pair<iterator, bool> Insert(const T& value)
        {
            return Emplace(value);
        }
        
        std::pair<iterator, bool> Insert(T&& value)
        {
            return Emplace(std::move(value));
        }
        
        template<typename... Args>
        std::pair<iterator, bool> Emplace(Args&&... args)
        {
            // For sets, we need to construct the value to check for duplicates
            // This is less efficient than maps but unavoidable
            alignas(T) std::uint8_t storage[sizeof(T)];
            T* temp = std::launder(reinterpret_cast<T*>(storage));
            std::allocator_traits<AllocatorType>::construct(m_alloc, temp, std::forward<Args>(args)...);
            
            // Use RAII to ensure cleanup
            struct Cleanup {
                AllocatorType& alloc;
                T* ptr;
                bool dismissed = false;
                ~Cleanup() { 
                    if (!dismissed) {
                        std::allocator_traits<AllocatorType>::destroy(alloc, ptr);
                    }
                }
            } cleanup{m_alloc, temp};
            
            ReserveForInsert();
            ASTRA_ASSERT(m_size + m_tombstoneCount < m_capacity, "Table should have space after reserve");
            
            auto [h1, h2] = SplitHash(m_hasher(*temp));
            SizeType index = SwissTable::H1(h1, m_capacity);
            SizeType probes = 0;
            // First empty-or-deleted slot seen on the probe path; the actual
            // insert is deferred until the duplicate scan has terminated.
            SizeType insertIdx = m_capacity;
            
            while (probes++ < m_capacity) ASTRA_LIKELY
            {
                SizeType groupIdx = index / GROUP_SIZE;
                SizeType startSlot = index % GROUP_SIZE;
                
                // First, check if value already exists
                std::uint16_t matches = m_groups[groupIdx].Match(h2);
                
                // Phase 1: Check existing values from start_slot to end of group
                std::uint16_t phase1Matches = matches >> startSlot;
                
                while (phase1Matches) ASTRA_UNLIKELY
                {
                    SizeType offset = Simd::Ops::CountTrailingZeros(phase1Matches);
                    SizeType slotIdx = startSlot + offset;
                    SizeType globalIdx = groupIdx * GROUP_SIZE + slotIdx;
                    
                    if (m_equal(*m_slots[globalIdx].GetValue(), *temp)) ASTRA_UNLIKELY
                    {
                        return {iterator(this, globalIdx), false}; // Value already exists
                    }
                    
                    phase1Matches &= phase1Matches - 1;
                }
                
                // Phase 2: Check existing values from beginning to start_slot
                if (startSlot > 0) ASTRA_LIKELY
                {
                    std::uint16_t phase2Mask = (1u << startSlot) - 1;
                    std::uint16_t phase2Matches = matches & phase2Mask;
                    
                    while (phase2Matches) ASTRA_UNLIKELY
                    {
                        SizeType slotIdx = Simd::Ops::CountTrailingZeros(phase2Matches);
                        SizeType globalIdx = groupIdx * GROUP_SIZE + slotIdx;
                        
                        if (m_equal(*m_slots[globalIdx].GetValue(), *temp)) ASTRA_UNLIKELY
                        {
                            return {iterator(this, globalIdx), false}; // Value already exists
                        }
                        
                        phase2Matches &= phase2Matches - 1;
                    }
                }
                
                // Remember the first empty-or-deleted slot on the probe path,
                // but do NOT insert yet: tombstones do not terminate the probe
                // chain, so the value may still exist in a later group. Inserting
                // at the first tombstone before the duplicate scan reaches an
                // EMPTY slot would double-insert the value.
                if (insertIdx == m_capacity)
                {
                    std::uint16_t emptyOrDeleted = m_groups[groupIdx].MatchEmptyOrDeleted();
                    std::uint16_t phase1Empty = emptyOrDeleted >> startSlot;
                    if (phase1Empty)
                    {
                        insertIdx = groupIdx * GROUP_SIZE + startSlot + Simd::Ops::CountTrailingZeros(phase1Empty);
                    }
                    else if (startSlot > 0)
                    {
                        std::uint16_t phase2Mask = static_cast<std::uint16_t>((1u << startSlot) - 1);
                        std::uint16_t phase2Empty = emptyOrDeleted & phase2Mask;
                        if (phase2Empty)
                        {
                            insertIdx = groupIdx * GROUP_SIZE + Simd::Ops::CountTrailingZeros(phase2Empty);
                        }
                    }
                }

                // An EMPTY slot terminates the probe chain: the value cannot live
                // beyond this group, so it is safe to insert now.
                if (m_groups[groupIdx].MatchEmpty() != 0) ASTRA_LIKELY
                {
                    break;
                }

                // No empty slot in this group, move to next
                groupIdx = (groupIdx + 1) % m_numGroups;
                index = groupIdx * GROUP_SIZE;

                // Prefetch next group
                Simd::Ops::PrefetchT0(&m_groups[groupIdx]);
            }

            // Should never trigger if ReserveForInsert() works correctly
            ASTRA_ASSERT(insertIdx < m_capacity, "FlatSet::Emplace failed to find insertion slot");
            if (insertIdx >= m_capacity) ASTRA_UNLIKELY
            {
                return {end(), false};
            }

            SizeType insertGroup = insertIdx / GROUP_SIZE;
            SizeType insertSlot = insertIdx % GROUP_SIZE;

            // Reusing a tombstone slot reduces the tombstone count
            if (m_groups[insertGroup].Get(insertSlot) == TOMBSTONE) ASTRA_UNLIKELY
            {
                --m_tombstoneCount;
            }

            // Move the value into place
            std::allocator_traits<AllocatorType>::construct(
                m_alloc,
                m_slots[insertIdx].GetValue(),
                std::move(*temp)
            );

            cleanup.dismissed = true; // Prevent cleanup since we moved the value

            m_groups[insertGroup].Set(insertSlot, h2);
            ++m_size;

            return {iterator(this, insertIdx), true};
        }
        
        template<typename K>
        SizeType Erase(const K& value)
        {
            auto it = Find(value);
            if (it == end()) ASTRA_UNLIKELY return 0;
            
            Erase(it);
            return 1;
        }
        
        iterator Erase(iterator pos)
        {
            SizeType idx = pos.m_index;
            SizeType groupIdx = idx / GROUP_SIZE;
            SizeType slotIdx = idx % GROUP_SIZE;
            
            std::allocator_traits<AllocatorType>::destroy(m_alloc, m_slots[idx].GetValue());
            m_groups[groupIdx].Set(slotIdx, TOMBSTONE);
            --m_size;
            ++m_tombstoneCount;
            
            return ++pos;
        }
        
        void Reserve(SizeType count)
        {
            if (count > m_capacity) ASTRA_UNLIKELY
            {
                SizeType newCapacity = NextPowerOfTwo(std::max(count, MIN_CAPACITY));
                Rehash(newCapacity);
            }
        }
        
        void Swap(FlatSet& other) noexcept
        {
            std::swap(m_groups, other.m_groups);
            std::swap(m_slots, other.m_slots);
            std::swap(m_capacity, other.m_capacity);
            std::swap(m_size, other.m_size);
            std::swap(m_tombstoneCount, other.m_tombstoneCount);
            std::swap(m_numGroups, other.m_numGroups);
            std::swap(m_hasher, other.m_hasher);
            std::swap(m_equal, other.m_equal);
            std::swap(m_alloc, other.m_alloc);
            std::swap(m_slotAlloc, other.m_slotAlloc);
            std::swap(m_groupAlloc, other.m_groupAlloc);
        }
        
    private:
        void AllocateStorage(SizeType capacity)
        {
            m_capacity = capacity;
            m_numGroups = (capacity + GROUP_SIZE - 1) / GROUP_SIZE;
            
            m_groups = m_groupAlloc.allocate(m_numGroups);
            m_slots = m_slotAlloc.allocate(capacity);
            
            // Initialize groups
            for (SizeType i = 0; i < m_numGroups; ++i)
            {
                std::allocator_traits<GroupAllocator>::construct(m_groupAlloc, &m_groups[i]);
            }
        }
        
        void DeallocateStorage() noexcept
        {
            if (m_groups) ASTRA_LIKELY
            {
                // Destroy all Group objects before deallocating
                for (SizeType i = 0; i < m_numGroups; ++i)
                {
                    std::allocator_traits<GroupAllocator>::destroy(m_groupAlloc, &m_groups[i]);
                }
                m_groupAlloc.deallocate(m_groups, m_numGroups);
                m_groups = nullptr;
            }
            if (m_slots) ASTRA_LIKELY
            {
                m_slotAlloc.deallocate(m_slots, m_capacity);
                m_slots = nullptr;
            }
            m_capacity = 0;
            m_numGroups = 0;
        }
        
        void ReserveForInsert()
        {
            if (m_capacity == 0) ASTRA_UNLIKELY
            {
                Rehash(MIN_CAPACITY);
            }
            else if (m_size + m_tombstoneCount + 1 > m_capacity * MAX_LOAD_FACTOR) ASTRA_UNLIKELY
            {
                Rehash(m_capacity * 2);
            }
            else if (m_tombstoneCount > m_capacity * 0.25f) ASTRA_UNLIKELY
            {
                // Trigger rehash when tombstones exceed 25%
                Rehash(m_capacity);
            }
        }
        
        void Rehash(SizeType newCapacity)
        {
            auto oldGroups = m_groups;
            auto oldSlots = m_slots;
            auto oldCapacity = m_capacity;
            auto oldNumGroups = m_numGroups;
            
            m_groups = nullptr;
            m_slots = nullptr;
            m_size = 0;
            m_tombstoneCount = 0;
            
            AllocateStorage(newCapacity);
            
            for (SizeType i = 0; i < oldCapacity; ++i)
            {
                SizeType groupIdx = i / GROUP_SIZE;
                SizeType slotIdx = i % GROUP_SIZE;
                
                std::uint8_t meta = oldGroups[groupIdx].Get(slotIdx);
                if (meta != EMPTY && meta != TOMBSTONE) ASTRA_UNLIKELY
                {
                    T* oldValue = oldSlots[i].GetValue();
                    
                    // Move the value to new location
                    Emplace(std::move(*oldValue));
                    
                    // Destroy the old value
                    std::allocator_traits<AllocatorType>::destroy(m_alloc, oldValue);
                }
            }
            
            // Destroy old groups before deallocating
            for (SizeType i = 0; i < oldNumGroups; ++i)
            {
                std::allocator_traits<GroupAllocator>::destroy(m_groupAlloc, &oldGroups[i]);
            }
            m_groupAlloc.deallocate(oldGroups, oldNumGroups);
            m_slotAlloc.deallocate(oldSlots, oldCapacity);
        }
        
        static constexpr SizeType NextPowerOfTwo(SizeType value) noexcept
        {
            static_assert(std::is_unsigned_v<SizeType>, "SizeType must be an unsigned integer type");
            if (value <= 1) ASTRA_UNLIKELY
            {
                return 1;
            }
            if (value > (std::numeric_limits<SizeType>::max() >> 1) + 1) ASTRA_UNLIKELY
            {
                return std::numeric_limits<SizeType>::max();
            }
            return std::bit_ceil(value);
        }
        
        // Use constants from Swiss.hpp
        static constexpr std::uint8_t EMPTY = SwissTable::EMPTY;
        static constexpr std::uint8_t DELETED = SwissTable::DELETED;
        static constexpr std::uint8_t TOMBSTONE = SwissTable::DELETED;  // Alias for compatibility
        static constexpr SizeType GROUP_SIZE = SwissTable::GROUP_SIZE;
        static constexpr float MAX_LOAD_FACTOR = SwissTable::MAX_LOAD_FACTOR;
        
        static constexpr SizeType MIN_CAPACITY = 16;
        
        template<typename K>
        SizeType FindImpl(const K& value, std::uint8_t& h2Out) const noexcept
        {
            if (Empty() || !m_groups) ASTRA_UNLIKELY return m_capacity;
            
            auto [h1, h2] = SplitHash(m_hasher(value));
            h2Out = h2;
            SizeType index = SwissTable::H1(h1, m_capacity);
            SizeType probes = 0;
            
            while (probes++ < m_capacity) ASTRA_LIKELY
            {
                SizeType groupIdx = index / GROUP_SIZE;
                SizeType startSlot = index % GROUP_SIZE;
                
                // Prefetch NEXT group's metadata early
                SizeType nextGroupIdx = (groupIdx + 1) % m_numGroups;
                Simd::Ops::PrefetchT1(&m_groups[nextGroupIdx]);
                
                // Get all matches in this group
                std::uint16_t matches = m_groups[groupIdx].Match(h2);
                
                // Prefetch slots if we have matches
                if (matches) ASTRA_UNLIKELY
                {
                    // Prefetch the first potential match slot
                    SizeType firstMatchIdx = Simd::Ops::CountTrailingZeros(matches);
                    SizeType prefetchIdx = groupIdx * GROUP_SIZE + firstMatchIdx;
                    Simd::Ops::PrefetchT0(&m_slots[prefetchIdx]);
                }
                
                // Phase 1: Check from start_slot to end
                std::uint16_t phase1Matches = matches >> startSlot;
                
                while (phase1Matches) ASTRA_UNLIKELY
                {
                    SizeType offset = Simd::Ops::CountTrailingZeros(phase1Matches);
                    SizeType slotIdx = startSlot + offset;
                    SizeType globalIdx = groupIdx * GROUP_SIZE + slotIdx;
                    
                    if (m_equal(*m_slots[globalIdx].GetValue(), value)) ASTRA_UNLIKELY
                    {
                        return globalIdx;
                    }
                    
                    phase1Matches &= phase1Matches - 1;
                }
                
                // Phase 2: Check from beginning to start_slot
                if (startSlot > 0) ASTRA_LIKELY
                {
                    std::uint16_t phase2Mask = (1u << startSlot) - 1;
                    std::uint16_t phase2Matches = matches & phase2Mask;
                    
                    while (phase2Matches) ASTRA_UNLIKELY
                    {
                        SizeType slotIdx = Simd::Ops::CountTrailingZeros(phase2Matches);
                        SizeType globalIdx = groupIdx * GROUP_SIZE + slotIdx;
                        
                        if (m_equal(*m_slots[globalIdx].GetValue(), value)) ASTRA_UNLIKELY
                        {
                            return globalIdx;
                        }
                        
                        phase2Matches &= phase2Matches - 1;
                    }
                }
                
                // Check for empty slots - if found, value doesn't exist
                std::uint16_t emptyMatches = m_groups[groupIdx].MatchEmpty();
                
                std::uint16_t relevantEmpty = (startSlot > 0)
                    ? emptyMatches
                    : (emptyMatches >> startSlot);
                
                if (relevantEmpty != 0) ASTRA_UNLIKELY
                {
                    return m_capacity; // Empty slot found, value doesn't exist
                }
                
                // Move to the next group
                groupIdx = nextGroupIdx;
                index = groupIdx * GROUP_SIZE;
            }
            
            return m_capacity;
        }
        
        // Extract H1 (57-bit position) and H2 (7-bit metadata) from hash
        static std::pair<std::size_t, std::uint8_t> SplitHash(std::size_t hash) noexcept
        {
            // Use SwissTable utilities for hash splitting
            std::uint8_t h2 = SwissTable::H2(hash);
            // Return full hash as h1, will be masked during probing
            return {hash, h2};
        }
        
        struct alignas(SIMD_ALIGNMENT) Group
        {
            // Aligned for SIMD operations (SSE/NEON use 128-bit registers)
            std::uint8_t metadata[GROUP_SIZE];
            
            Group() noexcept
            {
                std::memset(metadata, EMPTY, GROUP_SIZE);
            }
            
            std::uint16_t Match(std::uint8_t h2) const noexcept
            {
                ASTRA_ASSUME(h2 > 0 && h2 < 128);
                return Simd::Ops::MatchByteMask<Simd::Width128>(metadata, h2);
            }
            
            std::uint16_t MatchEmpty() const noexcept
            {
                return Simd::Ops::MatchByteMask<Simd::Width128>(metadata, EMPTY);
            }
            
            std::uint16_t MatchEmptyOrDeleted() const noexcept
            {
                return Simd::Ops::MatchEitherByteMask<Simd::Width128>(metadata, EMPTY, TOMBSTONE);
            }
            
            std::uint16_t MatchOccupied() const noexcept
            {
                return ~Simd::Ops::MatchEitherByteMask<Simd::Width128>(metadata, EMPTY, TOMBSTONE);
            }
            
            void Set(SizeType index, std::uint8_t value) noexcept
            {
                metadata[index] = value;
            }
            
            std::uint8_t Get(SizeType index) const noexcept
            {
                return metadata[index];
            }
            
            // Count occupied slots
            ASTRA_NODISCARD int CountOccupied() const noexcept
            {
                return Simd::Ops::PopCount(MatchOccupied());
            }
        };
        
        // Storage for values
        struct Slot
        {
            alignas(T) std::uint8_t storage[sizeof(T)];
            
            T* GetValue() noexcept
            {
                return std::launder(reinterpret_cast<T*>(storage));
            }
            
            const T* GetValue() const noexcept
            {
                return std::launder(reinterpret_cast<const T*>(storage));
            }
        };
        
        using SlotAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Slot>;
        using GroupAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Group>;
        
        Group* m_groups = nullptr;
        Slot* m_slots = nullptr;
        SizeType m_capacity = 0;
        SizeType m_size = 0;
        SizeType m_numGroups = 0;
        SizeType m_tombstoneCount = 0;
        Hasher m_hasher;
        KeyEqual m_equal;
        AllocatorType m_alloc;
        SlotAllocator m_slotAlloc;
        GroupAllocator m_groupAlloc;
    };
    
} // namespace Astra