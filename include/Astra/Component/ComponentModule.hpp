#pragma once

#include <memory>
#include <string>
#include <string_view>

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
                other.m_context  = nullptr;
                other.m_moduleId = 0;
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

        std::shared_ptr<ComponentRegistry> m_registry;
        TypeContext* m_context = nullptr;
        uint32_t m_moduleId = 0;
    };

    // Task-2 scope: release the handle. Task 3 adds the removal sweep here
    // (unregistering this module's owned descriptors from the registry).
    inline void ComponentModule::Reset()
    {
        m_registry.reset();
        m_context = nullptr;
        m_moduleId = 0;
    }
}
