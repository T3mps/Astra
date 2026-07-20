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
#include "Log.hpp"

#if defined(__cpp_rtti) || defined(_CPPRTTI)
#include <typeinfo>
#endif

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

    // Triviality bits carried by TypeIdentity.
    enum TypeIdentityFlags : uint8_t
    {
        TIF_TriviallyCopyable     = 1u << 0,
        TIF_TriviallyDestructible = 1u << 1,
        TIF_Empty                 = 1u << 2,
    };

    // Distinguishes two types that share a compiler pretty-name (and thus the
    // same stable name-hash) -- e.g. distinct same-named types in anonymous
    // namespaces across TUs. A pure side-channel used ONLY to detect a collision
    // at id assignment; it never enters the hash, the assigned id, or the wire
    // format. `size == 0` means "unspecified" (raw call with no type). Built by
    // MakeTypeIdentity<T>() (TypeID.hpp).
    struct TypeIdentity
    {
        uint32_t size  = 0;   // sizeof(T); always >= 1 for a real type, so 0 == unspecified
        uint32_t align = 0;   // alignof(T)
        uint8_t  flags = 0;   // TypeIdentityFlags bits
#if defined(__cpp_rtti) || defined(_CPPRTTI)
        const std::type_info* rtti = nullptr;  // &typeid(T) where RTTI is enabled
#endif
    };

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
        // On a name-hash collision (differing name, or a differing TypeIdentity for
        // the same name), refuses the second type: returns INVALID_COMPONENT and
        // logs an error instead of aliasing it onto the existing id.
        ASTRA_NODISCARD ComponentID GetOrAssignComponentID(uint64_t hash, std::string_view name,
                                                           TypeIdentity identity = {})
        {
            std::lock_guard lock(m_mutex);
            if (auto it = m_hashToId.Find(hash); it != m_hashToId.end())
            {
                const ComponentID existingId = it->second;
                // Two collision classes: (1) same name-hash + DIFFERENT name = a
                // real XXHash collision of two differently-named types; (2) same
                // name-hash + same name but a DIFFERENT type = identical
                // anonymous-namespace names across TUs (the discriminator catches
                // what the byte-identical name cannot). Either is refused loudly.
                if (m_names[existingId] != name
                    || IsTypeIdentityCollision(m_identities[existingId], identity))
                {
                    std::string msg = "TypeContext: type-identity collision -- incoming type '";
                    msg.append(name);
                    msg += "' shares the name-hash of already-registered '";
                    msg.append(m_names[existingId]);
                    msg += "'. The second type is refused (its ComponentID is INVALID). Give types a "
                           "unique unqualified name; do not place two same-named types in anonymous "
                           "namespaces across translation units.";
                    ASTRA_LOG_ERROR(msg);
                    ASTRA_ENSURE_ALWAYS(false, "TypeContext type-identity collision (see log)");
                    return INVALID_COMPONENT;  // refuse, uncached -- mirrors the id-exhaustion guard below
                }
                return existingId;
            }
            // All-config guard: uint16 id space exhausted. Refuse rather than
            // wrap m_next (which would silently alias a fresh type onto a
            // previously-assigned id). Not cached in the hash map, so every
            // call for this hash keeps refusing instead of caching a bogus
            // mapping. Must run BEFORE the ASTRA_ASSERT below so Debug builds
            // degrade the same way as Release/Dist (mirrors ComponentRegistry's
            // and FieldInfo.hpp's guard-before-assert ordering).
            if (m_next == INVALID_COMPONENT) ASTRA_UNLIKELY
            {
                return INVALID_COMPONENT;
            }
            ASTRA_ASSERT(m_next != INVALID_COMPONENT, "TypeContext ID space exhausted");
            const ComponentID id = m_next++;
            m_hashToId[hash] = id;
            m_names.emplace_back(name);
            m_identities.push_back(identity);
            return id;
        }

        // Defined inline in MetaRegistry.hpp (lazy-constructs the registry);
        // declared here against the forward declaration.
        ASTRA_NODISCARD MetaRegistry& Meta();

    private:
        // True iff a and b provably denote DIFFERENT types. Each dimension is
        // compared only when present on both sides. Absence of any comparable
        // dimension => not treated as a collision (raw test-helper calls).
        ASTRA_NODISCARD static bool IsTypeIdentityCollision(const TypeIdentity& a, const TypeIdentity& b) noexcept
        {
#if defined(__cpp_rtti) || defined(_CPPRTTI)
            if (a.rtti != nullptr && b.rtti != nullptr)
                return *a.rtti != *b.rtti;  // ABI-correct, cross-module-safe; disambiguates anon namespaces
#endif
            if (a.size != 0 && b.size != 0)
                return a.size != b.size || a.align != b.align || a.flags != b.flags;
            return false;
        }

        std::mutex m_mutex;      // guards id assignment (m_hashToId / m_names / m_next)
        FlatMap<uint64_t, ComponentID> m_hashToId;
        std::deque<std::string> m_names;  // index == id; collision diagnostics
        std::vector<TypeIdentity> m_identities;  // parallel to m_names, index == id
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
