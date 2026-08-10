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
    // code the descriptors should point into -- the compiler stamps each
    // descriptor's function pointers with THIS translation unit's code, so
    // the instantiating module is the module the resulting registration is
    // tied to (that is the whole point).
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
    //     (Reset() would erase it out from under its real owner on unload).
    //   - UnregisterModuleRange remains the fallback net for modules that
    //     never adopted this RAII path (or that forgot Shutdown cleanup) --
    //     it is a safety valve, not a substitute for the contract above.
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
        // Mixing Register<T> and RegisterMeta<T> for the SAME T on one module
        // is UNSUPPORTED. It fail-safes either way at teardown -- never
        // corrupting state -- but the two outcomes differ, so the mechanism is
        // worth stating exactly. Reset() runs the component sweep FIRST, then
        // EraseOwnedMetas:
        //   - Component slot CLEARED TO EMPTY: the sweep already called
        //     EraseUnchecked(hash), which removed the meta AND both link rows.
        //     The later guarded Erase(hash) in EraseOwnedMetas then finds
        //     nothing component-linked and nothing to erase, so it silently
        //     no-ops -- no log at all.
        //   - Component slot RESTORED to a survivor: the link row still maps
        //     hash -> id, so the guarded Erase REFUSES (it will not dangle a
        //     live descriptor's cached meta) and emits one ENSURE log. The
        //     survivor keeps its meta; only the spurious log is lost work.
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
        // ASTRA_REFLECT_TYPE'd AND over-aligned would otherwise commit a live,
        // component-LINKED TypeMeta into the shared MetaRegistry and only then
        // have its descriptor refused by Phase B: the meta and its hash<->id
        // link would then outlive the module FOREVER (ReleaseModule skips an id
        // whose m_present bit was never set, so the unload sweep never reaches
        // it, and the guarded MetaRegistry::Erase permanently refuses a
        // component-linked hash). Refusing up here means a refused type mints no
        // id, builds and links no meta, and installs no descriptor -- it is
        // invisible to the entire registry.
        // Phase B keeps its own re-test, and InstallOwned keeps its guard: those
        // are defense in depth on the DESCRIPTOR side (MakeDescriptor's contract
        // requires callers to re-test, and InstallOwned is public).
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
            // ORDER (meta BEFORE descriptor): the rebind must happen first so
            // the descriptor can capture the resulting TypeMeta*. An earlier
            // ordering built the descriptor from a PRE-rebind Meta().Get(),
            // which silently yielded meta == nullptr (and therefore
            // visitFields == nullptr, permanently) on the unload-before-load
            // reload path -- the previous generation's clear-to-empty had
            // already erased the entry, so there was nothing to Get() yet.
            //
            // LOCK DISCIPLINE: no lock NESTING occurs here. Phase A takes only
            // the MetaRegistry mutex (inside RebindInPlace/LinkToComponent);
            // phase B takes only the registry's registration mutex (inside
            // InstallOwned). They are held strictly sequentially, never
            // simultaneously, so the spec's "registration -> meta, never
            // reversed" rule -- which is about hold-and-wait nesting -- is not
            // in play at all. The thunk itself (arbitrary user reflection
            // code) runs with NO lock held.

            // ---- Phase A: meta (no registry lock) ----------------------------
            const TypeMeta* meta = nullptr;
            if (thunk)
            {
                TypeMeta fresh = thunk();
                // RebindInPlace is address-stable on the hit path (move-assign
                // through the existing unique_ptr) and installs fresh on the
                // miss path -- either way the returned pointer is the address
                // the descriptor must cache.
                meta = m_context->Meta().RebindInPlace(std::move(fresh));
                if (!meta)
                {
                    // Identity-collision refusal (RebindInPlace already logged
                    // + ENSUREd). The type's identity is contested, so do NOT
                    // install a descriptor claiming a meta we could not bind:
                    // treat it exactly like a refused id -- never owned.
                    return;
                }
                m_context->Meta().LinkToComponent(TypeID<T>::Hash(), id);
            }
            // An UNREFLECTED type (null thunk) still registers normally with
            // meta == nullptr; only a non-null thunk whose rebind REFUSED aborts.

            // ---- Phase B: descriptor + slot install --------------------------
            ComponentDescriptor desc = ComponentRegistry::MakeDescriptor<T>(id, meta);
            // MakeDescriptor refuses an over-aligned type by early-returning a
            // zeroed descriptor whose only valid field is `alignment`; callers
            // MUST re-test it. Mirrors ComponentRegistry.hpp's
            // RegisterComponentImpl guard -- chunk storage cannot honor
            // alignment above CACHE_LINE_SIZE, and an installed descriptor here
            // would be a live slot full of null function pointers.
            // UNREACHABLE via RegisterOne (the hoisted compile-time guard
            // already refused such a T before Phase A) -- kept as defense in
            // depth so this function stays correct on its own terms, and so a
            // future caller reaching it directly cannot skip the contract.
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

            m_registry->InstallOwned(id, m_moduleId, desc, thunk);
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
            // Record ownership ONLY if the rebind actually took. A refused
            // rebind (identity collision -> nullptr) left the INCUMBENT meta
            // in place; recording the hash anyway would make teardown erase
            // someone else's entry, destroying a meta this handle never owned.
            if (m_context->Meta().RebindInPlace(std::move(fresh)) != nullptr)
            {
                m_ownedMetas.push_back(TypeID<T>::Hash());
            }
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
                    // Scope caveat: this meta lives in the SHARED TypeContext, so a
                    // clear-to-empty in THIS registry erases a TypeMeta that another
                    // registry sharing the context may still cache in a live
                    // descriptor -- one registry per context is the supported shape
                    // for module-owned reflected components.
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
