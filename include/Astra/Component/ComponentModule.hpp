#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "../Container/SmallVector.hpp"
#include "../Core/TypeContext.hpp"
#include "../Core/TypeID.hpp"
#include "ComponentRegistry.hpp"

namespace Astra
{
    // RAII owner of a module's component registrations (spec 2026-08-09).
    // Open() captures the EXPLICITLY INSTALLED TypeContext -- all ids and
    // metas route through the captured context, never ambient per-module
    // state. Register<Ts...>() must be instantiated in the module whose
    // code the descriptors should point into (that is the whole point).
    class ComponentModule
    {
    public:
        ComponentModule() = default;

        ASTRA_NODISCARD static ComponentModule Open(std::shared_ptr<ComponentRegistry> registry,
                                                    std::string_view name)
        {
            ComponentModule mod;
            TypeContext* installed = Detail::CurrentTypeContextSlot();
            if (!registry || !ASTRA_ENSURE_ALWAYS(installed != nullptr,
                    "ComponentModule::Open requires SetTypeContext before use "
                    "(the module-local default context would silently mint private ids)"))
            {
                return mod;   // empty handle: observable refusal
            }
            mod.m_registry = std::move(registry);
            mod.m_context  = installed;
            mod.m_moduleId = mod.m_registry->OpenModuleId(name);
            return mod;
        }

        template<Component... Ts>
        void Register()
        {
            if (!*this) return;
            (RegisterOne<Ts>(), ...);
        }

        // Module-owned NON-component reflected types (Task 5). T is never
        // registered as a component here -- no ComponentID is consumed, no
        // descriptor phase, no LinkToComponent. The module still owns the
        // meta's lifetime: RebindInPlace points the closures into THIS
        // image, and the recorded hash is erased (via the guarded public
        // MetaRegistry::Erase) when the handle is reset/destroyed.
        template<typename... Ts>
        void RegisterMeta()
        {
            if (!*this) return;
            (RegisterOneMeta<Ts>(), ...);
        }

        void Reset();                      // Task 3 fills the removal sweep; Task 2: just release
        ~ComponentModule() { Reset(); }

        ComponentModule(ComponentModule&& other) noexcept { *this = std::move(other); }
        ComponentModule& operator=(ComponentModule&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                m_registry = std::move(other.m_registry);
                m_context  = other.m_context;
                m_moduleId = other.m_moduleId;
                m_ownedMetas = std::move(other.m_ownedMetas);
                other.m_context  = nullptr;
                other.m_moduleId = 0;
                other.m_ownedMetas.clear();
            }
            return *this;
        }
        ComponentModule(const ComponentModule&) = delete;
        ComponentModule& operator=(const ComponentModule&) = delete;

        ASTRA_NODISCARD explicit operator bool() const noexcept
        {
            return m_registry != nullptr && m_moduleId != 0;
        }

    private:
        template<Component T>
        void RegisterOne()
        {
            // Explicit-context id mint: TypeID<T>::Hash()/Name() are pure
            // compile-time values; only the id assignment touches context state.
            const ComponentID id = m_context->GetOrAssignComponentID(
                TypeID<T>::Hash(), TypeID<T>::Name(), MakeTypeIdentity<T>());
            if (id == INVALID_COMPONENT || id >= MAX_COMPONENTS)
                return;                                    // refused type: never owned

            MetaBuildFn thunk = Detail::MetaFactory<T>::fn ? &Detail::BuildMetaThunk<T> : nullptr;

            // Phase 1 (locked, inside registry): slot + owner bookkeeping.
            const TypeMeta* meta = m_context->Meta().Get(TypeID<T>::Hash());
            ComponentDescriptor desc = ComponentRegistry::MakeDescriptor<T>(id, meta);

            // MakeDescriptor is static (no ComponentRegistry instance), so it
            // cannot reach m_componentNames and leaves desc.name unset. This
            // module owns TypeID<T>::Name() -- a slice of the compiler's
            // __FUNCSIG__/__PRETTY_FUNCTION__ literal, not independently
            // NUL-terminated -- so copy it into a NUL-terminated buffer that
            // outlives the synchronous InstallOwned call below; InstallOwned
            // re-copies the bytes into the registry's own storage before
            // returning.
            const std::string nameStorage(TypeID<T>::Name());
            desc.name = nameStorage.c_str();

            if (!m_registry->InstallOwned(id, m_moduleId, desc, thunk))
                return;

            // Phase 2 (NO locks held here): rebuild + install this module's
            // meta so field closures point into THIS image (spec §3.3).
            if (thunk)
            {
                TypeMeta fresh = thunk();
                m_context->Meta().RebindInPlace(std::move(fresh));
                m_context->Meta().LinkToComponent(TypeID<T>::Hash(), id);
            }
        }

        // Mirrors RegisterOne's phase 2 ONLY -- T is not a component, so
        // there is no id mint, no descriptor, no InstallOwned/LinkToComponent.
        // Requires T reflected in THIS module (Detail::MetaFactory<T>::fn);
        // otherwise refuses observably and skips, matching RegisterOne's
        // treat-refusal-as-safe-no-op contract.
        template<typename T>
        void RegisterOneMeta()
        {
            if (!*this) return;
            if (!Detail::MetaFactory<T>::fn)
            {
                ASTRA_ENSURE_ALWAYS(false, "RegisterMeta<T> requires T reflected in this module");
                return;
            }

            MetaBuildFn thunk = &Detail::BuildMetaThunk<T>;   // built outside locks

            TypeMeta fresh = thunk();
            m_context->Meta().RebindInPlace(std::move(fresh));
            m_ownedMetas.push_back(TypeID<T>::Hash());
        }

        // Erases every meta this handle owns via RegisterMeta (guarded public
        // Erase -- non-component hashes always pass the component-linked
        // check, so this never refuses in the legitimate path). Called from
        // Reset() BEFORE the registry/context members are released.
        void EraseOwnedMetas()
        {
            for (uint64_t hash : m_ownedMetas)
            {
                m_context->Meta().Erase(hash);
            }
            m_ownedMetas.clear();
        }

        std::shared_ptr<ComponentRegistry> m_registry;
        TypeContext* m_context = nullptr;
        uint32_t m_moduleId = 0;
        SmallVector<uint64_t, 4> m_ownedMetas;
    };

    // Full unload semantics (Task 3). A no-op on a moved-from or
    // already-reset handle (operator bool guards the sweep). ReleaseModule
    // does the locked bookkeeping and hands back the meta work; that work
    // runs here, OUTSIDE any registry lock, because a MetaBuildFn thunk (or
    // MetaRegistry's own mutex) is user reflection code the registry lock
    // must not be held across.
    inline void ComponentModule::Reset()
    {
        if (*this)
        {
            auto metaWork = m_registry->ReleaseModule(m_moduleId);
            for (auto& w : metaWork)          // outside all locks: user reflect code
            {
                if (w.buildMeta)              // survivor restored: rebind its meta
                {
                    TypeMeta fresh = w.buildMeta();
                    m_context->Meta().RebindInPlace(std::move(fresh));
                    m_context->Meta().LinkToComponent(w.hash, w.id);
                }
                else                          // cleared to empty: meta goes too
                {
                    m_context->Meta().EraseUnchecked(w.hash);
                }
            }
            // Module-owned non-component metas (RegisterMeta, Task 5): erase
            // BEFORE releasing m_context -- EraseOwnedMetas needs it valid.
            EraseOwnedMetas();
        }
        m_registry.reset();
        m_context = nullptr;
        m_moduleId = 0;
    }
}
