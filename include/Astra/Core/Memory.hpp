#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include "../Core/Base.hpp"

#ifdef ASTRA_PLATFORM_WINDOWS
    #include <windows.h>
    #include <memoryapi.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <cstring>
    #ifdef ASTRA_PLATFORM_LINUX
        // memfd.h may not be available on all Linux systems (e.g., WSL)
        #if __has_include(<sys/memfd.h>)
            #include <sys/memfd.h>
        #endif
        #if __has_include(<linux/memfd.h>)
            #include <linux/memfd.h>
        #endif
    #elif defined(ASTRA_PLATFORM_MACOS)
        #include <sys/shm.h>
    #endif
#endif

namespace Astra
{
    // Cache line size constants for proper alignment
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t DESTRUCTIVE_INTERFERENCE = std::hardware_destructive_interference_size;
    inline constexpr std::size_t CONSTRUCTIVE_INTERFERENCE = std::hardware_constructive_interference_size;
#else
    // Platform-specific fallbacks when C++17 hardware_interference_size is not available
#if defined(ASTRA_ARCH_X64) || defined(ASTRA_ARCH_X86)
    inline constexpr std::size_t DESTRUCTIVE_INTERFERENCE = 64;
    inline constexpr std::size_t CONSTRUCTIVE_INTERFERENCE = 64;
#elif defined(ASTRA_ARCH_ARM64)
    inline constexpr std::size_t DESTRUCTIVE_INTERFERENCE = 128;
    inline constexpr std::size_t CONSTRUCTIVE_INTERFERENCE = 64;
#else
    inline constexpr std::size_t DESTRUCTIVE_INTERFERENCE = 64;
    inline constexpr std::size_t CONSTRUCTIVE_INTERFERENCE = 64;
#endif
#endif

    // Cache line size - typically used for alignment
    inline constexpr std::size_t CACHE_LINE_SIZE = std::max(DESTRUCTIVE_INTERFERENCE, CONSTRUCTIVE_INTERFERENCE);

    // SIMD alignment requirements
    inline constexpr std::size_t SIMD_ALIGNMENT = 16;

    // Default page size for memory allocation
    inline constexpr std::size_t DEFAULT_PAGE_SIZE = 4096; // 4KB
    inline constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;  // 2MB on Windows

    enum class AllocFlags : uint32_t
    {
        None = 0,
        HugePages = 1 << 0,  // Use 2MB/1GB huge pages if available
        ZeroMem = 1 << 1,  // Zero-initialize allocated memory
    };
    
    ASTRA_FORCEINLINE constexpr AllocFlags operator|(AllocFlags a, AllocFlags b) noexcept
    {
        return static_cast<AllocFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    
    ASTRA_FORCEINLINE constexpr bool operator&(AllocFlags a, AllocFlags b) noexcept
    {
        return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
    }
    
    struct AllocResult
    {
        void* ptr = nullptr;
        size_t size = 0;
        bool usedHugePages = false;
    };
    
    ASTRA_FORCEINLINE bool IsHugePagesAvailable() noexcept
    {
        static bool checked = false;
        static bool available = false;
        
        if (!checked)
        {
            #ifdef ASTRA_PLATFORM_WINDOWS
                HANDLE token;
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                {
                    LUID luid;
                    if (LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &luid))
                    {
                        PRIVILEGE_SET privSet = {};
                        privSet.PrivilegeCount = 1;
                        privSet.Control = PRIVILEGE_SET_ALL_NECESSARY;
                        privSet.Privilege[0].Luid = luid;
                        privSet.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;
                        
                        BOOL result;
                        PrivilegeCheck(token, &privSet, &result);
                        available = (result == TRUE);
                    }
                    CloseHandle(token);
                }
            #else
                FILE* f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
                if (f)
                {
                    char buffer[256];
                    if (fgets(buffer, sizeof(buffer), f))
                    {
                        available = (strstr(buffer, "[always]") != nullptr || 
                                   strstr(buffer, "[madvise]") != nullptr);
                    }
                    fclose(f);
                }
            #endif
            
            checked = true;
        }
        
        return available;
    }
    
    ASTRA_FORCEINLINE AllocResult AllocateMemory(size_t size, size_t alignment = 64, AllocFlags flags = AllocFlags::None) noexcept
    {
        AllocResult result;
        result.size = size;
        
        size = (size + alignment - 1) & ~(alignment - 1);
        
        bool tryHugePages = (flags & AllocFlags::HugePages) && IsHugePagesAvailable();
        bool zeroMemory = (flags & AllocFlags::ZeroMem);
        
        #ifdef ASTRA_PLATFORM_WINDOWS
            if (tryHugePages && size >= HUGE_PAGE_SIZE)
            {
                size_t hugePagesSize = (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
                
                void* ptr = VirtualAlloc(nullptr, hugePagesSize, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
                    
                if (ptr)
                {
                    result.ptr = ptr;
                    result.size = hugePagesSize;
                    result.usedHugePages = true;
                    return result;
                }
            }
            
            void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (ptr)
            {
                result.ptr = ptr;
                result.size = size;
                
                if (zeroMemory)
                {
                    std::memset(ptr, 0, size);
                }
            }
        #else
            int prot = PROT_READ | PROT_WRITE;
            int flags_mmap = MAP_PRIVATE | MAP_ANONYMOUS;
            
            if (tryHugePages && size >= HUGE_PAGE_SIZE)
            {
                size_t hugePagesSize = (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
                
                // Try different huge page sizes
                #ifdef MAP_HUGE_2MB
                    void* ptr = mmap(nullptr, hugePagesSize, prot,
                        flags_mmap | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
                    if (ptr != MAP_FAILED)
                    {
                        result.ptr = ptr;
                        result.size = hugePagesSize;
                        result.usedHugePages = true;
                        
                        // Advise kernel about huge pages if not already using them
                        #ifdef MADV_HUGEPAGE
                            madvise(ptr, hugePagesSize, MADV_HUGEPAGE);
                        #endif
                        
                        if (zeroMemory)
                        {
                            std::memset(ptr, 0, hugePagesSize);
                        }
                        return result;
                    }
                #endif
                
                // Try generic huge pages
                void* ptr = mmap(nullptr, hugePagesSize, prot,
                    flags_mmap | MAP_HUGETLB, -1, 0);
                if (ptr != MAP_FAILED)
                {
                    result.ptr = ptr;
                    result.size = hugePagesSize;
                    result.usedHugePages = true;
                    
                    if (zeroMemory)
                    {
                        std::memset(ptr, 0, hugePagesSize);
                    }
                    return result;
                }
            }
            
            // Fall back to regular allocation with alignment
            void* ptr = nullptr;
            if (alignment > sizeof(void*))
            {
                // Use aligned allocation
                if (posix_memalign(&ptr, alignment, size) == 0)
                {
                    result.ptr = ptr;
                    result.size = size;
                    
                    // Advise kernel about huge pages even for regular allocation
                    #ifdef MADV_HUGEPAGE
                        if (tryHugePages && size >= HUGE_PAGE_SIZE)
                        {
                            madvise(ptr, size, MADV_HUGEPAGE);
                        }
                    #endif
                    
                    if (zeroMemory)
                    {
                        std::memset(ptr, 0, size);
                    }
                }
            }
            else
            {
                // Regular allocation
                ptr = std::malloc(size);
                if (ptr)
                {
                    result.ptr = ptr;
                    result.size = size;
                    
                    if (zeroMemory)
                    {
                        std::memset(ptr, 0, size);
                    }
                }
            }
        #endif
        
        return result;
    }
    
    /**
     * Free memory allocated with AllocateMemory
     */
    ASTRA_FORCEINLINE void FreeMemory(void* ptr, size_t size, bool usedHugePages = false) noexcept
    {
        if (!ptr) return;
        
        #ifdef ASTRA_PLATFORM_WINDOWS
            (void)size;
            (void)usedHugePages;
            VirtualFree(ptr, 0, MEM_RELEASE);
        #else
            if (usedHugePages)
            {
                munmap(ptr, size);
            }
            else
            {
                std::free(ptr);
            }
        #endif
    }
}