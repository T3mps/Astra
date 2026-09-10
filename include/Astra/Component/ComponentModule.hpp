#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "../Container/SmallVector.hpp"
#include "../Core/ModuleIdentity.hpp"
#include "../Core/TypeContext.hpp"
#include "../Core/TypeID.hpp"
#include "ComponentRegistry.hpp"

namespace Astra
{
    // RAII owner of a module's component registrations (spec 2026-08-09;
    // meta lifetime per spec 2026-09-09). Open() captures the EXPLICITLY
    // INSTALLED TypeContext and this module's identity -- all ids and metas
    // route through the captured context, never ambient per-module state.
    // Register<Ts...>() must be instantiated in the module whose code the
    // descriptors should point into -- the compiler stamps each descriptor's
    // function pointers with THIS translation unit's code, so the
    // instantiating module is the module the resulting registration is tied
    // to (that is the whole point).
    //
    // Ownership contract (mandatory, not advisory):
    //   - HEAP-HELD in every plugin/module that uses it (e.g. a file-scope
    //     std::optional<ComponentModule>), and reset EXPLICITLY from that
    //     module's own Shutdown entry point.
    //   - NEVER a plugin-side static/global object. A DLL static's
    //     destructor runs *during* FreeLibrary at DLL_PROCESS_DETACH, under
    //     the loader lock -- but ~ComponentModule() takes the registry's
    //     registration mutex, may invoke a MetaBuildFn thunk (arbitrary user
    //     reflection code), and may drop the last shared_ptr to the
    //     registry. None of that is safe under the loader lock. Heap-held +
    //     explicit Shutdown-time Reset() keeps all of it off the loader lock.
    //   - "Register only what you own": Register<Ts...>() should list only
    //     the types this module's code actually implements -- registering a
    //     type you don't own just to keep it alive defeats RAII ownership
    //     (Reset() would release it out from under its real owner on unload).
    //   - UnregisterModuleRange remains the fallback net for modules that
    //     never adopted this RAII path (or that forgot Shutdown cleanup) --
    //     it is a safety valve, not a substitute for the contract above.
    //
    // Several registries per context are supported: each registration holds
    // its own ref on the type's meta binder, and a meta is erased only when
    // no registry, no handle, and no resident module holds it any more
    // (spec 2026-09-09 §3.3).
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
            // Birth-context affinity: the installed slot must be the context
            // the target registry's ids are minted under, or every id this
            // module registers would come from the wrong counter.
            if (!ASTRA_ENSURE_ALWAYS(installed == registry->GetBirthContext(),
                    "ComponentModule::Open: this module's installed TypeContext is not the "
                    "one the target registry was constructed under -- ids would alias"))
            {
                return mod;   // empty handle: observable refusal
            }
            mod.m_registry = std::move(registry);
            mod.m_context  = installed;
            mod.m_identity = Detail::CurrentModuleIdentity();   // token + residency of THIS image
            mod.m_moduleId = mod.m_registry->OpenModuleId(name);
            return mod;
        }

        template<Component... Ts>
        void Register()
        {
            if (!*this) return;
            (RegisterOne<Ts>(), ...);
        }

        // Module-owned NON-component reflected types. T is never registered as
        // a component here -- no ComponentID is consumed, no descriptor phase,
        // no LinkToComponent. The module still owns the meta's lifetime: the
        // bind points the closures into THIS image and takes one ref; Reset()
        // releases it. Register<T> and RegisterMeta<T> for the SAME T on one
        // handle is fine: both are refs on the same binder and both are
        // released at teardown (the slot's by ReleaseModule, this one by
        // ReleaseOwnedMetas).
        template<typename... Ts>
        void RegisterMeta()
        {
            if (!*this) return;
            (RegisterOneMeta<Ts>(), ...);
        }

        void Reset();
        ~ComponentModule() { Reset(); }

        ComponentModule(ComponentModule&& other) noexcept { *this = std::move(other); }
        ComponentModule& operator=(ComponentModule&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                m_registry   = std::move(other.m_registry);
                m_context    = other.m_context;
                m_identity   = other.m_identity;
                m_moduleId   = other.m_moduleId;
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
        // The descriptor alignment MakeDescriptor will compute for T. Mirrors
        // its expression EXACTLY, empty-type case included, so the module path
        // refuses precisely the set of types the anonymous path refuses --
        // no more, no less (an over-aligned EMPTY tag is storage-free and is
        // accepted by both).
        template<Component T>
        static constexpr size_t DescriptorAlignmentV = std::is_empty_v<T> ? size_t(1) : alignof(T);

        // Over-aligned refusal, HOISTED above EVERY side effect. Alignment is a
        // purely compile-time property -- it needs neither a ComponentID nor a
        // TypeMeta -- so this MUST precede Phase A. A type that is both
        // ASTRA_REFLECT_TYPE'd AND over-aligned would otherwise push a binder
        // and take a ref it could never release (the slot is never installed,
        // so ReleaseModule never reaches it). Refusing up here means a refused
        // type mints no id, binds no meta, and installs no descriptor -- it is
        // invisible to the entire registry.
        template<Component T>
        void RegisterOne()
        {
            if constexpr (DescriptorAlignmentV<T> <= CACHE_LINE_SIZE)
            {
                RegisterOneChecked<T>();
            }
            // else: over-aligned -> refused with zero side effects.
        }

        template<Component T>
        void RegisterOneChecked()
        {
            // Explicit-context id mint: TypeID<T>::Hash()/Name() are pure
            // compile-time values; only the id assignment touches context state.
            const ComponentID id = m_context->GetOrAssignComponentID(
                TypeID<T>::Hash(), TypeID<T>::Name(), MakeTypeIdentity<T>());
            if (id == INVALID_COMPONENT || id >= MAX_COMPONENTS)
                return;                                    // refused type: never owned

            MetaBuildFn thunk = Detail::MetaFactory<T>::fn ? &Detail::BuildMetaThunk<T> : nullptr;

            // Not atomic across two modules racing the same type -- benign:
            // reload registration is host-serialized by contract.
            //
            // LOCK DISCIPLINE: no lock NESTING occurs here. Phase A takes only
            // the MetaRegistry mutex (inside Bind/LinkToComponent); phase B
            // takes only the registry's registration mutex (inside
            // InstallOwned); phase C takes only the MetaRegistry mutex again.
            // They are held strictly sequentially, never simultaneously. The
            // thunk itself (arbitrary user reflection code) runs with NO lock
            // held.

            // ---- Phase A: meta bind (no registry lock) ----------------------
            const TypeMeta* meta = nullptr;
            if (thunk)
            {
                TypeMeta fresh = thunk();
                // Bind installs when the entry is absent (unload-before-load
                // reload: the previous generation's last release erased it),
                // swaps content to `fresh` when this module's binder is top,
                // and leaves content alone when another module is above us.
                // Either way the returned pointer is the address the descriptor
                // must cache (spec 2026-09-09 §3.5 flow 3).
                const BindResult bound = m_context->Meta().Bind(TypeID<T>::Hash(), m_identity, thunk, &fresh);
                if (bound.outcome == BindOutcome::Refused)
                {
                    // Identity-collision refusal (Bind already logged + ENSUREd).
                    // The type's identity is contested, so do NOT install a
                    // descriptor claiming a meta we could not bind: treat it
                    // exactly like a refused id -- never owned.
                    return;
                }
                meta = bound.meta;
                m_context->Meta().LinkToComponent(TypeID<T>::Hash(), id);
            }
            // An UNREFLECTED type (null thunk) still registers normally with
            // meta == nullptr; only a non-null thunk whose bind REFUSED aborts.

            // ---- Phase B: descriptor + slot install --------------------------
            ComponentDescriptor desc = ComponentRegistry::MakeDescriptor<T>(id, meta);
            // MakeDescriptor refuses an over-aligned type by early-returning a
            // zeroed descriptor whose only valid field is `alignment`; callers
            // MUST re-test it. UNREACHABLE via RegisterOne (the hoisted
            // compile-time guard already refused such a T before Phase A) --
            // kept as defense in depth so this function stays correct on its
            // own terms.
            if (desc.alignment > CACHE_LINE_SIZE) ASTRA_UNLIKELY
                return;                                    // over-aligned: never owned

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

            const InstallResult installed = m_registry->InstallOwned(id, m_moduleId, desc, m_identity.token);

            // ---- Phase C: ref accounting ------------------------------------
            // A new slot or an override push holds one ref on this module's
            // binder; a same-owner in-place replace already holds it.
            if (meta && (installed == InstallResult::Installed || installed == InstallResult::Overrode))
            {
                m_context->Meta().Acquire(TypeID<T>::Hash(), m_identity.token);
            }
        }

        // Mirrors RegisterOne's Phase A ONLY -- T is not a component, so there
        // is no id mint, no descriptor, no InstallOwned/LinkToComponent.
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
            // Record ownership ONLY if the bind and the acquire both took. A
            // refused bind (identity collision) left the INCUMBENT meta in
            // place; recording the hash anyway would make teardown release a
            // ref this handle never held.
            if (m_context->Meta().Bind(TypeID<T>::Hash(), m_identity, thunk, &fresh).outcome != BindOutcome::Refused
                && m_context->Meta().Acquire(TypeID<T>::Hash(), m_identity.token))
            {
                m_ownedMetas.push_back(TypeID<T>::Hash());
            }
        }

        // Releases every ref this handle took via RegisterMeta. Called from
        // Reset() BEFORE the registry/context members are released.
        void ReleaseOwnedMetas()
        {
            for (uint64_t hash : m_ownedMetas)
            {
                m_context->Meta().Release(hash, m_identity.token);
            }
            m_ownedMetas.clear();
        }

        std::shared_ptr<ComponentRegistry> m_registry;
        TypeContext* m_context = nullptr;
        Detail::ModuleIdentity m_identity{};
        uint32_t m_moduleId = 0;
        SmallVector<uint64_t, 4> m_ownedMetas;
    };

    // Full unload semantics. A no-op on a moved-from or already-reset handle
    // (operator bool guards the sweep). ReleaseModule does the locked slot
    // bookkeeping and hands back one release per departing entry; those
    // releases run here, OUTSIDE any registry lock, because MetaRegistry::
    // Release may run a MetaBuildFn thunk (user reflection code) to rebind
    // the meta to whichever module survives on top. If this module still
    // holds refs from another registry, its binder stays and nothing
    // rebinds; if its last ref goes, the stack rebinds to the survivor or --
    // when nothing else holds the type -- erases the meta (spec 2026-09-09
    // §3.5 flow 4).
    inline void ComponentModule::Reset()
    {
        if (*this)
        {
            auto work = m_registry->ReleaseModule(m_moduleId);
            for (const auto& w : work)        // outside all locks: may run user reflect code
            {
                m_context->Meta().Release(w.hash, w.module);
            }
            // Module-owned non-component metas: release BEFORE dropping
            // m_context -- ReleaseOwnedMetas needs it valid.
            ReleaseOwnedMetas();
        }
        m_registry.reset();
        m_context = nullptr;
        m_moduleId = 0;
    }
}
