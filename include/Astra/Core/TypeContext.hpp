#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../Component/Component.hpp"
#include "../Container/FlatMap.hpp"
#include "Base.hpp"

// NOTE: deliberately does NOT include MetaRegistry.hpp -- MetaRegistry's
// templated API uses TypeID, and TypeID.hpp includes this header. The meta
// registry is held through a forward declaration; the accessor body is
// defined in MetaRegistry.hpp (which includes this header). Include order
// is: TypeID.hpp -> TypeContext.hpp <- MetaRegistry.hpp (no cycle).

namespace Astra
{
    class MetaRegistry;  // see layering note above
    class TypeContext;

    namespace Detail
    {
        // Static registrars run during module load, before any host can call
        // SetTypeContext -- they enqueue here (module-local by design) and
        // are drained into a context on install (SetTypeContext) or on first
        // MetaRegistry::Instance() access in standalone use.
        using PendingMetaRegistration = std::function<void(TypeContext&)>;

        inline std::mutex& PendingMetaMutex()
        {
            static std::mutex s_mutex;
            return s_mutex;
        }

        // NOT thread-safe: caller must hold PendingMetaMutex() or be in a
        // single-threaded context (tests only). Production code enqueues via
        // EnqueuePendingMeta, which takes the queue mutex.
        inline std::vector<PendingMetaRegistration>& PendingMetaQueue()
        {
            static std::vector<PendingMetaRegistration> s_queue;
            return s_queue;
        }

        inline void EnqueuePendingMeta(PendingMetaRegistration registration)
        {
            std::lock_guard lock(PendingMetaMutex());
            PendingMetaQueue().push_back(std::move(registration));
        }

        void DrainPendingMeta(TypeContext& ctx);  // defined below TypeContext
    }

    // Process-wide type identity service. Component/type IDs are assigned
    // densely (ComponentMask bit index == ComponentID) keyed by the STABLE
    // XXHash64 type-name hash, so every module (EXE/DLL) that shares one
    // TypeContext agrees on IDs. Hosts create one context and hand it to
    // each plugin module via SetTypeContext() BEFORE that module touches
    // any Registry/TypeID API (per-type IDs are cached in per-module
    // statics and will not re-resolve afterwards).
    class TypeContext
    {
    public:
        // Not noexcept: allocates on first sight of a hash.
        ASTRA_NODISCARD ComponentID GetOrAssignComponentID(uint64_t hash, std::string_view name)
        {
            std::lock_guard lock(m_mutex);
            if (auto it = m_hashToId.Find(hash); it != m_hashToId.end())
            {
#ifdef ASTRA_BUILD_DEBUG
                ASTRA_ASSERT(m_names[it->second] == name,
                             "TypeContext hash collision: two distinct type names share a hash");
#endif
                return it->second;
            }
            ASTRA_ASSERT(m_next != INVALID_COMPONENT, "TypeContext ID space exhausted");
            const ComponentID id = m_next++;
            m_hashToId[hash] = id;
            m_names.emplace_back(name);
            return id;
        }

        // Defined inline in MetaRegistry.hpp (lazy-constructs the registry);
        // declared here against the forward declaration.
        ASTRA_NODISCARD MetaRegistry& Meta();

    private:
        std::mutex m_mutex;      // guards id assignment (m_hashToId / m_names / m_next)
        FlatMap<uint64_t, ComponentID> m_hashToId;
        std::deque<std::string> m_names;  // index == id; collision diagnostics
        ComponentID m_next = 0;
        std::mutex m_metaMutex;  // guards m_meta lazy init only; separate so meta
                                 // access never contends with id assignment
        std::shared_ptr<MetaRegistry> m_meta;  // shared_ptr: deleter bound where type is complete
    };

    namespace Detail
    {
        inline TypeContext*& CurrentTypeContextSlot() noexcept
        {
            static TypeContext* s_ctx = nullptr;  // per-module slot, by design
            return s_ctx;
        }
    }

    // The module-default context (created lazily for standalone use).
    inline TypeContext& DefaultTypeContext()
    {
        static TypeContext s_ctx;
        return s_ctx;
    }

    // Install the process-shared context for THIS module, draining any
    // pending static meta registrations into it. Must run before the
    // module's first TypeID<T>::Value() / Registry use. Passing nullptr
    // uninstalls (reverts to DefaultTypeContext) and performs no drain.
    // Not noexcept: drained registration callbacks may allocate.
    inline void SetTypeContext(TypeContext* ctx)
    {
        Detail::CurrentTypeContextSlot() = ctx;
        if (ctx)
        {
            Detail::DrainPendingMeta(*ctx);
        }
    }

    // Pure resolver: returns the installed context, or the module-default
    // when none is installed. Does NOT drain pending meta registrations --
    // draining happens in SetTypeContext (host install) and in
    // MetaRegistry::Instance() (standalone first meta access), so that
    // enqueued-but-not-yet-installed plugin registrations are not absorbed
    // into the default context by an unrelated TypeID lookup.
    ASTRA_NODISCARD inline TypeContext* GetTypeContext()
    {
        TypeContext* ctx = Detail::CurrentTypeContextSlot();
        return ctx ? ctx : &DefaultTypeContext();
    }

    namespace Detail
    {
        inline void DrainPendingMeta(TypeContext& ctx)
        {
            // Swap-and-run: callbacks execute outside the queue lock, so a
            // callback that (indirectly) enqueues or re-enters a drain cannot
            // deadlock; the outer loop picks up anything enqueued meanwhile.
            for (;;)
            {
                std::vector<PendingMetaRegistration> batch;
                {
                    std::lock_guard lock(PendingMetaMutex());
                    auto& queue = PendingMetaQueue();
                    if (queue.empty())
                    {
                        return;
                    }
                    batch.swap(queue);
                }
                for (auto& registration : batch)
                {
                    registration(ctx);
                }
            }
        }
    }
}
