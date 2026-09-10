#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>

#include "../Container/FlatMap.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Result.hpp"
#include "../Core/TypeID.hpp"
#include "../Reflection/MetaRegistry.hpp"
#include "../Reflection/FieldVisitor.hpp"
#include "../Serialization/BinaryArchive.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "Component.hpp"

namespace Astra
{
    // Outcome of ComponentRegistry::InstallOwned (spec 2026-09-09 §3.4).
    enum class InstallResult : uint8_t { Refused, Installed, Overrode, Replaced };

    class ComponentRegistry
    {
    public:
        ComponentRegistry()
        {
            // m_hashToID (hash -> id) holds at most MAX_COMPONENTS entries and is
            // never erased; reserve past the ceiling so registration never
            // rehashes it. Descriptor POINTER stability is guaranteed structurally
            // by m_components being a fixed directly-indexed array (see the member
            // declaration), so GetComponentDescriptor() pointers stay valid for the
            // registry's life no matter how many types register.
            m_hashToID.Reserve(MAX_COMPONENTS * 2);
        }

        // A type whose identity collided with an already-registered type (see
        // TypeContext::GetOrAssignComponentID) resolves to INVALID_COMPONENT and
        // is refused here -- no descriptor is written for it.
        template<Component T>
        void RegisterComponent()
        {
            // Birth-context affinity (all-config): a caller whose ambient context
            // differs from the one this registry was constructed under would mint
            // ids from the WRONG counter -- the silent cross-module aliasing
            // class. Checked BEFORE TypeID<T>::Value() so the mismatched module's
            // per-type id cache is never seeded from the wrong context.
            if (!ASTRA_ENSURE_ALWAYS(GetTypeContext() == m_birthContext,
                    "ComponentRegistry: RegisterComponent from a module whose TypeContext "
                    "differs from this registry's birth context -- call Astra::SetTypeContext "
                    "in the calling module first"))
                return;
            const ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY  // guard before indexing m_registered
                return;
            if (m_registered[id].load(std::memory_order_acquire))
                return;                                // warm path: lock-free
            ModuleToken rebuildAs = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_registrationMutex);
                if (m_registered[id].load(std::memory_order_relaxed))
                    return;                            // double-check under lock
                rebuildAs = RegisterComponentImpl<T>(id);   // may refuse (over-aligned) -- that's fine
                m_registered[id].store(true, std::memory_order_release);  // "attempt resolved for id"
            }
            if (rebuildAs != nullptr)
            {
                // This module's binder just became the TOP binder for a type
                // some other module drained: the content must carry this
                // module's closures. Built here, OUTSIDE the registration lock
                // (user reflection code), and swapped only if still top
                // (spec 2026-09-09 §3.4). The token is the one the bind was
                // made under, threaded out rather than re-read.
                TypeMeta fresh = Detail::BuildMetaThunk<T>();
                MetaRegistry::Instance().Rebind(TypeID<T>::Hash(), rebuildAs, std::move(fresh));
            }
        }

        // Bulk, single-threaded setup path: do NOT call from workers. Each
        // per-type RegisterComponent() below re-locks m_registrationMutex, so
        // this must NOT hold the lock across the fan-out (std::mutex is
        // non-recursive -- that would deadlock).
        template<Component... Components>
        void RegisterComponents()
        {
            constexpr size_t count = sizeof...(Components);
            if (count == 0) return;

            (RegisterComponent<Components>(), ...);
        }

        // Module-unload safety valve.
        //
        // A ComponentDescriptor stores raw function pointers (defaultConstruct,
        // destruct, visitFields, ...) that live in whichever MODULE registered the
        // type -- and a module may re-point a type at itself via
        // ComponentModule::Register — the RAII path cleans that up itself; this
        // range purge is the fallback net for modules that never adopted it.
        // When that module is unloaded, every such descriptor is left aiming at
        // freed code. Worse, the first-registration guard in RegisterComponent
        // means the type can never be rebuilt: m_registered[id] is still true, so
        // a reloaded module's RegisterComponent<T> early-returns and the stale
        // pointers survive. The next call through one is an access violation.
        //
        // Call this with the unloading module's image range BEFORE freeing it.
        // Every descriptor whose code lies inside is dropped, so that:
        //   - a lookup before anything re-registers returns nullptr -- callers get
        //     a clean "not registered" miss instead of calling into freed memory;
        //   - a later RegisterComponent<T> takes the cold path and rebuilds the
        //     descriptor against whichever module is live at that point.
        //
        // Deliberately ADDRESS-RANGE based rather than tracking an owning module
        // handle: that keeps Astra free of any platform module API, and the caller
        // is by definition the one that knows the range it is about to unmap.
        //
        // Unified with the shadow owner stack (Task 7): a purge no longer just
        // blanks the live slot. For every id it first strips any SHADOWED entry
        // whose descriptor code lies in range -- probed the same way as the live
        // entry, and regardless of whether the live entry itself is in range,
        // because a shadowed entry from a long-gone non-RAII module must not
        // outlive it just because something else is live above it now. Then, if
        // the LIVE entry is in range, it is replaced through the same
        // RestoreOrClearSlot path ReleaseModule uses: the newest remaining
        // shadow entry is restored rather than the slot being unconditionally
        // blanked, so a module that overrode another and is later force-unloaded
        // through this fallback net still falls back correctly -- exactly like
        // the RAII path.
        //
        // A restored shadow entry's module is still mapped only under RAII
        // discipline; a host that unmapped a non-RAII module without purging it
        // first double-faults here -- that ordering is the documented contract.
        //
        // m_hashToID is intentionally left alone -- GetComponentDescriptorByHash
        // resolves through GetComponentDescriptor, which is gated on m_present, so
        // clearing the presence bit already makes the lookup miss. Re-registration
        // simply overwrites the same hash -> id entry with the same id.
        //
        // Meta work runs through the ambient MetaRegistry::Instance() AFTER the
        // registration lock is released. That is correct here specifically
        // because the caller of a range purge is, by construction, the HOST --
        // the module that installed the shared TypeContext -- and never the
        // plugin being unloaded. Each dropped entry is released against ITS
        // OWN module's binder (the token recorded at registration), so one
        // purge that hits several modules' entries releases each correctly
        // (spec 2026-09-09 §3.5 flow 5).
        //
        // Returns the number of descriptors dropped (shadowed + live).
        size_t UnregisterModuleRange(const void* base, size_t size)
        {
            if (!base || size == 0)
                return 0;

            const uintptr_t lo = reinterpret_cast<uintptr_t>(base);
            const uintptr_t hi = lo + size;
            auto inRange = [lo, hi](uintptr_t addr)
            {
                return addr != 0 && addr >= lo && addr < hi;
            };
            // RegisterComponentImpl/ComponentModule::RegisterOne instantiate all
            // of these together in one module, so any single hit proves
            // ownership. Several are tested rather than just one because
            // copyConstruct/visitFields are conditionally null, and a descriptor
            // must not be missed merely because the slot this check happened to
            // pick was the null one. Shared between the live-entry check and the
            // shadow-entry probe below.
            auto isOwned = [&inRange](const ComponentDescriptor& d)
            {
                return inRange(reinterpret_cast<uintptr_t>(d.defaultConstruct)) ||
                       inRange(reinterpret_cast<uintptr_t>(d.destruct)) ||
                       inRange(reinterpret_cast<uintptr_t>(d.moveConstruct)) ||
                       inRange(reinterpret_cast<uintptr_t>(d.moveAssign)) ||
                       inRange(reinterpret_cast<uintptr_t>(d.serialize)) ||
                       inRange(reinterpret_cast<uintptr_t>(d.visitFields));
            };

            SmallVector<MetaRelease, 4> metaWork;
            size_t dropped = 0;
            {
                std::lock_guard<std::mutex> lock(m_registrationMutex);
                for (size_t id = 0; id < MAX_COMPONENTS; ++id)
                {
                    // Strip in-range SHADOW entries for this id first, regardless
                    // of whether the live entry is owned by this range.
                    if (auto it = m_shadow.Find(static_cast<ComponentID>(id)); it != m_shadow.end())
                    {
                        auto& list = it->second;
                        for (size_t i = list.size(); i-- > 0;)
                        {
                            if (isOwned(list[i].desc))
                            {
                                if (list[i].desc.meta)
                                    metaWork.push_back(MetaRelease{list[i].desc.hash, list[i].module});
                                list.erase(list.begin() + static_cast<ptrdiff_t>(i));
                                ++dropped;
                            }
                        }
                        if (list.empty()) m_shadow.Erase(it);
                    }

                    if (!m_present.Test(id) || !isOwned(m_components[id]))
                        continue;

                    // Live entry is in range: replace it via the same
                    // restore-newest-shadow-or-clear path ReleaseModule uses
                    // (rather than unconditionally blanking). m_owner[id] and
                    // m_metaModule[id] are updated inside RestoreOrClearSlot.
                    RestoreOrClearSlot(static_cast<ComponentID>(id), metaWork);
                    ++dropped;
                }
            }

            for (const auto& w : metaWork)         // outside the lock: may run user reflection code
            {
                MetaRegistry::Instance().Release(w.hash, w.module);
            }

            return dropped;
        }

        ASTRA_NODISCARD const ComponentDescriptor* GetComponentDescriptor(ComponentID id) const
        {
            // Directly-indexed, pointer-stable: &m_components[id] never moves for
            // the registry's lifetime, so callers may cache it safely.
            return (id < MAX_COMPONENTS && m_present.Test(id)) ? &m_components[id] : nullptr;
        }
        
        ASTRA_NODISCARD const ComponentDescriptor* GetComponentDescriptorByHash(uint64_t hash) const
        {
            auto hashIt = m_hashToID.Find(hash);
            if (hashIt == m_hashToID.end())
                return nullptr;
            return GetComponentDescriptor(hashIt->second);
        }
        
        ASTRA_NODISCARD Result<ComponentID, std::string_view> GetComponentIDFromHash(uint64_t hash) const
        {
            auto it = m_hashToID.Find(hash);
            if (it == m_hashToID.end())
                return Result<ComponentID, std::string_view>::Err("Unknown component hash");
            return Result<ComponentID, std::string_view>::Ok(it->second);
        }

        // Invokes fn(ComponentID, const ComponentDescriptor&) for every registered
        // component in ascending id order. Replaces the former GetAllComponentIDs()
        // that leaked the internal container (and its unstable FlatMap iterators).
        template<typename Fn>
        void ForEachComponent(Fn&& fn) const
        {
            for (size_t id = 0; id < MAX_COMPONENTS; ++id)
                if (m_present.Test(id))
                    fn(static_cast<ComponentID>(id), m_components[id]);
        }

        // Birth-context affinity anchor: the TypeContext this registry's ids
        // are minted under (captured at construction, in the constructing
        // module). Registry's Debug accessor tripwire and
        // ComponentModule::Open's all-config refusal both compare against it.
        ASTRA_NODISCARD TypeContext* GetBirthContext() const noexcept { return m_birthContext; }

        ASTRA_NODISCARD size_t Size() const
        {
            return m_present.Count();
        }

        // Introduced for registration-lifecycle tests + AstraStudio's registry
        // panel; counts distinct stored name strings, not registered components.
        ASTRA_NODISCARD size_t ComponentNameCount() const
        {
            return m_componentNames.size();
        }

        void GetAllDescriptors(std::vector<ComponentDescriptor>& descriptors) const
        {
            descriptors.clear();
            descriptors.reserve(m_present.Count());
            for (size_t id = 0; id < MAX_COMPONENTS; ++id)
                if (m_present.Test(id))
                    descriptors.push_back(m_components[id]);
        }

        // ==== ComponentModule plumbing (spec 2026-08-09) ====================

        // THE descriptor-alignment rule -- the single definition both
        // registration paths and MakeDescriptor read, so the anonymous path
        // and the module path refuse EXACTLY the same set of types, no more
        // and no less. An empty/tag type gets 1: it has no column, so its
        // declared alignment is storage-irrelevant and an over-aligned empty
        // tag stays acceptable to both paths.
        template<Component T>
        static constexpr size_t DescriptorAlignmentV = std::is_empty_v<T> ? size_t(1) : alignof(T);

        // Pure descriptor build: size/alignment/triviality/hash/version/fn-pointers,
        // extracted from RegisterComponentImpl so ComponentModule can build the
        // same descriptor for an OWNED registration. Static (no `this`): does not
        // touch m_componentNames/m_components/m_present/m_hashToID -- desc.name
        // is left unset; the caller stores it via StoreComponentName. `meta` is
        // whatever the caller already resolved (may be null); this function never
        // queries MetaRegistry itself, so it has no opinion on which TypeContext
        // is in play.
        template<Component T>
        ASTRA_NODISCARD static ComponentDescriptor MakeDescriptor(ComponentID id, const TypeMeta* meta)
        {
            // Value-initialized, NOT default-initialized: the over-aligned
            // refusal below returns early, and every field after that point
            // would otherwise be INDETERMINATE. Zeroing up front makes a
            // refused descriptor all-null/all-zero (null fn pointers, null
            // name, zero hash) instead of garbage, so a caller that forgets
            // to re-test desc.alignment fails loudly rather than silently
            // calling into an uninitialized function pointer.
            ComponentDescriptor desc{};
            desc.id = id;
            // Empty components should report size 0 to avoid memory allocation
            desc.size = std::is_empty_v<T> ? 0 : sizeof(T);
            desc.alignment = DescriptorAlignmentV<T>;
            // All-config guard: chunk storage can only honor alignments up to
            // CACHE_LINE_SIZE; refuse registration so failure is observable
            // (descriptor lookup returns nullptr; Registry::AddComponent returns
            // nullptr) instead of handing back a descriptor the storage will
            // misalign. Must run BEFORE the ASTRA_ASSERT below so Debug builds
            // degrade the same way as Release/Dist (mirrors FieldInfo.hpp's
            // Get/Set/GetPtr guard-before-assert ordering). Callers detect this
            // refusal by re-testing desc.alignment (already valid at this point)
            // before installing -- everything after this guard is unset.
            if (desc.alignment > CACHE_LINE_SIZE) ASTRA_UNLIKELY
            {
                return desc;
            }
            ASTRA_ASSERT(desc.alignment <= CACHE_LINE_SIZE,
                         "Component alignment above 64 bytes is not supported by chunk storage");

            desc.hash = TypeID<T>::Hash();

            desc.version = SerializationTraits<T>::Version;
            desc.minVersion = SerializationTraits<T>::MinVersion;

            desc.is_trivially_copyable = std::is_trivially_copyable_v<T>;
            desc.is_copy_constructible = std::is_copy_constructible_v<T>;
            desc.is_nothrow_move_constructible = std::is_nothrow_move_constructible_v<T>;
            desc.is_nothrow_default_constructible = std::is_nothrow_default_constructible_v<T>;
            desc.is_trivially_default_constructible = std::is_trivially_default_constructible_v<T>;
            desc.is_trivially_destructible = std::is_trivially_destructible_v<T>;
            desc.is_empty = std::is_empty_v<T>;
            desc.isEnableable = IsEnableableV<T>;

            // An enableable component must carry storage: the disabled bit lives
            // per-entity in the chunk's column data, and an empty/tag type has no
            // column (idToColumn == -1) to hang that bit off of. A runtime guard
            // would make such a type silently unfilterable instead of catching the
            // mistake at the call site, so refuse it at compile time.
            static_assert(!(std::is_empty_v<T> && IsEnableableV<T>),
                "An enableable component must have storage: an empty/tag type has no column to carry "
                "a disabled bit. Use a non-empty component, or a separate enableable marker with a byte of state.");

            desc.defaultConstruct = &DefaultConstruct<T>;
            desc.destruct = &Destruct<T>;
            desc.moveConstruct = &MoveConstruct<T>;
            desc.moveAssign = &MoveAssign<T>;

            if constexpr (std::is_copy_constructible_v<T>)
            {
                desc.copyConstruct = &CopyConstruct<T>;
                desc.copyAssign = &CopyAssign<T>;
                desc.constructWith = &ConstructWith<T>;
            }
            else
            {
                desc.copyConstruct = nullptr;
                desc.copyAssign = nullptr;
                desc.constructWith = nullptr;
            }

            desc.serialize = &Serialize<T>;
            desc.deserialize = &Deserialize<T>;
            desc.serializeVersioned = &SerializeVersioned<T>;
            desc.deserializeVersioned = &DeserializeVersioned<T>;

            // Link to reflection metadata (resolved by the caller, not looked up here).
            desc.meta = meta;

            // Reflection-driven visitor slot: null unless the type is reflected.
            desc.visitFields = desc.meta ? &VisitFields<T> : nullptr;

            return desc;
        }

        // Issues module ids starting at 1 (0 means anonymous/unowned). Thread-safe.
        uint32_t OpenModuleId(std::string_view name)
        {
            std::lock_guard<std::mutex> lock(m_registrationMutex);
            m_moduleNames.emplace_back(name);
            return m_nextModuleId++;
        }

        ASTRA_NODISCARD uint32_t GetOwner(ComponentID id) const
        {
            return (id < MAX_COMPONENTS) ? m_owner[id] : 0u;
        }

        // Snapshot of a slot's previously-live entry, taken when a different
        // owner overrides it (InstallOwned) so ReleaseModule can restore it.
        // `module` is the token the entry's meta ref was acquired under.
        struct ShadowEntry { uint32_t owner; ComponentDescriptor desc; ModuleToken module; };
        // Post-lock meta work ReleaseModule / UnregisterModuleRange hand back
        // to their caller: one Release per departing live-or-shadowed entry
        // that held a meta ref (spec 2026-09-09 §3.4). The meta stack decides
        // what a release means (held / rebound / erased); the registry only
        // reports the departure.
        struct MetaRelease { uint64_t hash; ModuleToken module; };

        // ComponentModule plumbing. Entry contract: desc.name must be a live
        // NUL-terminated const char* for the duration of this call -- it is
        // re-pointed to registry-owned storage under the lock before this
        // function returns (see the by-value-desc paragraph below), so the
        // caller's storage need not outlive the call, only span it.
        // Full semantics:
        //   1. id >= MAX_COMPONENTS -> Refused.
        //   2. Slot empty            -> Installed (plain install).
        //   3. Live owner == owner   -> Replaced: in-place replace of the live
        //                               entry (no shadow: this module is just
        //                               re-registering/rebinding its own type).
        //                               REFUSED instead if that replace would
        //                               flip desc.meta's nullness -- see below.
        //   4. Live owner != owner   -> Overrode: the CURRENT live entry
        //                               (owner, desc, module) is pushed onto
        //                               this id's shadow stack before the new
        //                               entry overwrites the slot, so a later
        //                               ReleaseModule can restore it.
        // The caller acquires a meta ref on Installed and Overrode only; on
        // Replaced the ref already exists and the image is the same. That last
        // clause only holds while the slot's meta NULLNESS is unchanged: a
        // same-owner replace that flips it (registered before the type had a
        // factory, then reflected, then registered again -- or the reverse)
        // would leave RestoreOrClearSlot emitting a release nobody acquired, or
        // swallowing one that was taken. Within one registry that is merely
        // loud (ENSURE + Unbound), but the stray decrement lands on the binder
        // keyed by the SAME token, so it can drop ANOTHER registry's ref and
        // erase a meta that is still cached. The flip is refused all-config.
        // By-value desc: desc.name is caller-owned temporary storage (RegisterOne's
        // stack std::string's c_str()) and is copied into registry-owned storage
        // here, under the lock, before it is written into any slot. A pushed
        // SHADOW copy needs no such re-storage: it is a copy of the CURRENT live
        // entry, whose desc.name already points at registry-owned m_componentNames
        // storage from that entry's own prior InstallOwned call.
        // Returns Refused when the id is invalid, or when the descriptor is
        // over-aligned.
        InstallResult InstallOwned(ComponentID id, uint32_t owner, ComponentDescriptor desc, ModuleToken module)
        {
            // Birth-context affinity (all-config) -- see RegisterComponent.
            if (!ASTRA_ENSURE_ALWAYS(GetTypeContext() == m_birthContext,
                    "ComponentRegistry: InstallOwned from a foreign-context module"))
                return InstallResult::Refused;
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
            {
                return InstallResult::Refused;
            }
            // Defense in depth: this is a PUBLIC entry point, so it repeats
            // MakeDescriptor's over-alignment refusal instead of trusting every
            // caller to re-test it (RegisterComponentImpl and
            // ComponentModule::RegisterOne both do, but a hand-built descriptor
            // need not have come through MakeDescriptor at all). Chunk storage
            // can only honor alignments up to CACHE_LINE_SIZE; installing a
            // descriptor above it would hand back misaligned component memory.
            if (desc.alignment > CACHE_LINE_SIZE) ASTRA_UNLIKELY
            {
                return InstallResult::Refused;
            }

            // The override notice is BUILT under the lock (DescribeOverride
            // reads m_moduleNames and the live slot's name) but EMITTED after
            // the lock releases: ASTRA_LOG_INFO reaches a user-installed
            // LogSink, and user code must never run under m_registrationMutex.
            std::string overrideNotice;
            bool refusedReplace = false;
            InstallResult result = InstallResult::Installed;
            {
                std::lock_guard<std::mutex> lock(m_registrationMutex);
                // std::string_view from a null pointer is UB, and a hand-built
                // descriptor may legitimately arrive with desc.name unset.
                desc.name = StoreComponentName(desc.name ? desc.name : "");

                if (m_present.Test(id))
                {
                    if (m_owner[id] != owner)
                    {
                        overrideNotice = DescribeOverride(owner, m_owner[id], m_components[id].name);
                        m_shadow[id].push_back(ShadowEntry{m_owner[id], m_components[id], m_metaModule[id]});
                        result = InstallResult::Overrode;
                    }
                    else
                    {
                        // Ref parity: a same-owner replace takes NO new meta
                        // ref, which is only sound while the slot's meta
                        // nullness is unchanged (see the contract above). The
                        // DECISION is made here, under the lock; the ENSURE
                        // that reports it runs after the lock scope closes --
                        // its failure handler is user code, exactly like the
                        // override notice's log sink.
                        refusedReplace = ((m_components[id].meta != nullptr) != (desc.meta != nullptr));
                        result = InstallResult::Replaced;
                    }
                }

                if (!refusedReplace)   // refused: the slot is left untouched
                {
                    m_components[id] = desc;
                    m_present.Set(id);
                    m_hashToID[desc.hash] = id;
                    m_owner[id] = owner;
                    m_metaModule[id] = module;
                    m_registered[id].store(true, std::memory_order_release);
                }
            }
            if (refusedReplace)
            {
                ASTRA_ENSURE_ALWAYS(false,
                    "InstallOwned: same-owner replace changed meta nullness -- ref accounting would desync");
                return InstallResult::Refused;
            }
            if (!overrideNotice.empty())
            {
                ASTRA_LOG_INFO(overrideNotice);
            }
            return result;
        }

        // Per-owner cleanup for module unload (ComponentModule::Reset). Under
        // m_registrationMutex: strips this owner's SHADOWED entries wherever
        // they sit in the stack, then -- for every id where this owner's entry
        // is LIVE -- either restores the newest remaining shadow entry or clears
        // the slot to empty. One MetaRelease per departing entry that held a
        // meta ref is returned rather than performed here so the caller can
        // run it OUTSIDE all locks (MetaRegistry::Release may run a MetaBuildFn
        // thunk -- user reflection code).
        SmallVector<MetaRelease, 4> ReleaseModule(uint32_t owner)
        {
            SmallVector<MetaRelease, 4> metaWork;
            std::lock_guard<std::mutex> lock(m_registrationMutex);
            for (size_t id = 0; id < MAX_COMPONENTS; ++id)
            {
                // Drop this owner's SHADOWED entries wherever they sit.
                if (auto it = m_shadow.Find(static_cast<ComponentID>(id)); it != m_shadow.end())
                {
                    auto& list = it->second;
                    for (size_t i = list.size(); i-- > 0;)
                    {
                        if (list[i].owner == owner)
                        {
                            if (list[i].desc.meta)
                                metaWork.push_back(MetaRelease{list[i].desc.hash, list[i].module});
                            list.erase(list.begin() + static_cast<ptrdiff_t>(i));
                        }
                    }
                    if (list.empty()) m_shadow.Erase(it);
                }
                if (!m_present.Test(id) || m_owner[id] != owner)
                    continue;
                // This owner's entry is LIVE: restore newest shadow, or clear.
                RestoreOrClearSlot(static_cast<ComponentID>(id), metaWork);
            }
            return metaWork;
        }

    private:
        // Replaces the LIVE entry at `id` with the newest remaining shadow
        // entry, or clears the slot to empty if none remain -- recording the
        // DEPARTING entry's meta release in `metaWork` rather than performing
        // it here, so callers can run it OUTSIDE the registration lock. Must
        // be called while m_registrationMutex is already held. Shared by
        // ReleaseModule (owner-scoped release) and UnregisterModuleRange
        // (address-range purge). No meta rebind happens here: the survivor's
        // module is already a binder on the meta stack, and Release rebinds
        // to it if the departing binder was on top (spec 2026-09-09 §3.4).
        // See UnregisterModuleRange's doc comment above for the RAII-discipline
        // / double-fault contract a restored shadow entry depends on.
        void RestoreOrClearSlot(ComponentID id, SmallVector<MetaRelease, 4>& metaWork)
        {
            if (m_components[id].meta)
                metaWork.push_back(MetaRelease{m_components[id].hash, m_metaModule[id]});

            if (auto it = m_shadow.Find(id); it != m_shadow.end() && !it->second.empty())
            {
                ShadowEntry restored = std::move(it->second.back());
                it->second.pop_back();
                if (it->second.empty()) m_shadow.Erase(it);
                m_components[id]  = restored.desc;        // same address, new contents
                m_owner[id]       = restored.owner;
                m_metaModule[id]  = restored.module;
            }
            else
            {
                m_components[id] = ComponentDescriptor{};
                m_present.Reset(id);
                m_owner[id] = 0;
                m_metaModule[id] = nullptr;
                m_registered[id].store(false, std::memory_order_release);
            }
        }

        // Builds the ASTRA_LOG_INFO override-notice body for InstallOwned's
        // different-owner branch. `componentName` may be null (defensive only --
        // the live slot always has a name once m_present is set).
        std::string DescribeOverride(uint32_t newOwner, uint32_t prevOwner, const char* componentName) const
        {
            auto nameOf = [this](uint32_t o) -> std::string_view
            {
                return (o != 0 && static_cast<size_t>(o - 1) < m_moduleNames.size())
                    ? std::string_view(m_moduleNames[o - 1])
                    : std::string_view("(anonymous)");
            };
            std::string msg = "ComponentModule: module '";
            msg += nameOf(newOwner);
            msg += "' overrides '";
            msg += nameOf(prevOwner);
            msg += "' for component '";
            msg += componentName ? componentName : "";
            msg += "'";
            return msg;
        }

        // Returns the module token the caller must Rebind under after
        // dropping the registration lock (BoundNeedsRebuild -- this module's
        // binder became the top binder for a type some other module drained),
        // or nullptr when no rebuild is needed or the type was refused.
        template<Component T>
        ModuleToken RegisterComponentImpl(ComponentID id)
        {
            ASTRA_ASSERT(id < MAX_COMPONENTS,
                         "Component ID space exhausted (MAX_COMPONENTS); raise ASTRA_MAX_COMPONENTS");
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
            {
                // Refuse registration so failure is observable (descriptor
                // lookup returns nullptr; Registry::AddComponent returns
                // nullptr) instead of silently corrupting ComponentMask bits.
                return nullptr;
            }

            // Over-aligned refusal BEFORE any meta side effect (mirrors
            // ComponentModule's hoisted gate): a refused type must push no
            // binder. MakeDescriptor repeats the test as defense in depth.
            if constexpr (DescriptorAlignmentV<T> > CACHE_LINE_SIZE)
            {
                return nullptr;
            }
            else
            {
                // Anonymous registration binds under THIS module's identity
                // (the template is instantiated in the caller's module, so
                // CurrentModuleIdentity() is the caller's copy) and takes one
                // ref for this slot (spec 2026-09-09 §3.4). No thunk runs here:
                // we are under m_registrationMutex.
                const Detail::ModuleIdentity who = Detail::CurrentModuleIdentity();
                const MetaBuildFn thunk = Detail::MetaFactory<T>::fn ? &Detail::BuildMetaThunk<T> : nullptr;
                const TypeMeta* meta = nullptr;
                bool needsRebuild = false;
                {
                    // A Refused bind (type reflected in some other module but
                    // never drained into this context, and no factory here)
                    // registers the component UNREFLECTED -- meta null, no link,
                    // no ref -- exactly what a null Get<T>() produced before.
                    const BindResult bound = MetaRegistry::Instance().Bind(TypeID<T>::Hash(), who, thunk, nullptr);
                    if (bound.outcome != BindOutcome::Refused)
                    {
                        meta = bound.meta;
                        needsRebuild = (bound.outcome == BindOutcome::BoundNeedsRebuild);
                    }
                }

                ComponentDescriptor desc = MakeDescriptor<T>(id, meta);
                if (desc.alignment > CACHE_LINE_SIZE) ASTRA_UNLIKELY  // MakeDescriptor refused (unreachable: gated above)
                {
                    return nullptr;
                }

                // Pointer-stable c_str() storage, reused by content -- see
                // StoreComponentName (a rebuilt/reloaded type's name string is
                // identical to what's already stored, so this does not grow).
                desc.name = StoreComponentName(TypeID<T>::Name());

                // Link MetaRegistry to ComponentID for reverse lookup and take
                // this slot's ref on the binder Bind just ensured.
                if (desc.meta)
                {
                    MetaRegistry::Instance().LinkToComponent(desc.hash, id);
                    MetaRegistry::Instance().Acquire(desc.hash, who.token);
                }

                // Directly indexed; the array slot is pointer-stable for life.
                m_components[id] = desc;
                m_present.Set(id);
                m_hashToID[desc.hash] = id;
                m_owner[id] = 0;  // anonymous: this path never goes through a ComponentModule
                m_metaModule[id] = who.token;
                return needsRebuild ? who.token : nullptr;
            }
        }

        // Copies `name` into pointer-stable, NUL-terminated storage and returns
        // the stored copy's address. Shared by RegisterComponentImpl and
        // InstallOwned; both call this while already holding m_registrationMutex
        // (std::mutex is non-recursive, so this must NOT lock itself).
        const char* StoreComponentName(std::string_view name)
        {
            // Reuse an existing entry by content before appending. A hot-reload
            // loop re-registers the SAME names over and over (RegisterComponentImpl
            // on every fresh id, InstallOwned on every module (re)registration),
            // so this bounds m_componentNames growth by distinct type names
            // instead of unbounded per-registration growth. The deque is tiny,
            // so a linear scan is the right cost/complexity trade here.
            for (const std::string& existing : m_componentNames)
            {
                if (existing == name)
                    return existing.c_str();
            }
            m_componentNames.emplace_back(name);
            return m_componentNames.back().c_str();
        }

        template<typename T>
        static void DefaultConstruct(void* ptr)
        {
            new (ptr) T{};
        }

        template<typename T>
        static void Destruct(void* ptr)
        {
            static_cast<T*>(ptr)->~T();
        }

        template<typename T>
        static void CopyConstruct(void* dst, const void* src)
        {
            new (dst) T(*static_cast<const T*>(src));
        }

        template<typename T>
        static void MoveConstruct(void* dst, void* src)
        {
            new (dst) T(std::move(*static_cast<T*>(src)));
        }

        template<typename T>
        static void MoveAssign(void* dst, void* src)
        {
            *static_cast<T*>(dst) = std::move(*static_cast<T*>(src));
        }

        template<typename T>
        static void CopyAssign(void* dst, const void* src)
        {
            *static_cast<T*>(dst) = *static_cast<const T*>(src);
        }
        
        template<typename T>
        static void ConstructWith(void* dst, const void* src)
        {
            // Use placement new with copy constructor for optimal construction
            new (dst) T(*static_cast<const T*>(src));
        }

        template<typename T>
        static void Serialize(BinaryWriter& writer, void* ptr)
        {
            T* component = static_cast<T*>(ptr);
            writer(*component);
        }

        template<typename T>
        static void Deserialize(BinaryReader& reader, void* ptr)
        {
            T* component = static_cast<T*>(ptr);
            reader(*component);
        }

        template<typename T>
        static void SerializeVersioned(BinaryWriter& writer, void* ptr)
        {
            T* component = static_cast<T*>(ptr);
            writer.WriteVersionedComponent(*component);
        }

        template<typename T>
        static bool DeserializeVersioned(BinaryReader& reader, void* ptr)
        {
            T* component = static_cast<T*>(ptr);
            auto result = reader.ReadVersionedComponent(*component);
            return result.IsOk();
        }

        template<typename T>
        static void VisitFields(void* instance, IFieldVisitor& visitor)
        {
            const TypeMeta* meta = MetaRegistry::Instance().Get<T>();
            if (!meta) return;
            for (const FieldInfo& field : meta->fields)
            {
                if (!field.IsSerializable()) continue;  // honors Serializable(false)
                visitor.Visit(field, instance);
            }
        }

        // Descriptors live in a directly-indexed, POINTER-STABLE array: ComponentID
        // is a dense 0..MAX_COMPONENTS-1 index (ComponentMask bit == ComponentID),
        // so a hash map keyed by it was both slower and unstable -- a FlatMap rehash
        // moved the slot a cached descriptor pointer referenced, dangling it (the
        // ResourceStorage teardown segfault). A fixed array never moves, so every
        // GetComponentDescriptor() pointer stays valid for the registry's life.
        std::array<ComponentDescriptor, MAX_COMPONENTS> m_components{};
        ComponentMask m_present;  // which ids in m_components hold a registered descriptor
        FlatMap<uint64_t, ComponentID> m_hashToID;  // hash -> id (sparse; stays a hash map)
        // Use deque instead of vector to prevent pointer invalidation when adding new names.
        // Vector reallocation would invalidate all c_str() pointers stored in ComponentDescriptor::name
        std::deque<std::string> m_componentNames;

        // First-registration guard. m_registered[id] is set once per type; the
        // warm path is a lock-free acquire-load (replacing the old Contains()
        // lookup). m_registrationMutex serializes the one-time cold path so two
        // workers first-registering different types can't race the containers.
        // non-copyable: holds a registration mutex + atomics (ComponentRegistry
        // is only ever held via std::shared_ptr; never copied/moved by value).
        // The TypeContext this registry was constructed under (NSDMI: captured
        // at construction, in whichever module constructs it). See
        // GetBirthContext() -- the affinity guard for cross-module aliasing.
        TypeContext* m_birthContext = GetTypeContext();

        std::mutex m_registrationMutex;
        std::atomic<bool> m_registered[MAX_COMPONENTS] = {};

        // ComponentModule plumbing. m_owner tracks who owns the LIVE entry at
        // each id (0 = anonymous, i.e. registered via RegisterComponent, never
        // through a module); m_metaModule records the module token the LIVE
        // entry's meta ref was acquired under, so the departing entry can be
        // released against the right binder (spec 2026-09-09 §3.4). Both are
        // written only under m_registrationMutex (RegisterComponentImpl /
        // InstallOwned / RestoreOrClearSlot). NOTE: the registry destructor
        // releases NOTHING -- anonymous entries outlive their registry exactly
        // as before (registry-less GetMeta readers depend on it); over-holding
        // is the safe direction (spec §3.6 c).
        uint32_t m_owner[MAX_COMPONENTS] = {};
        ModuleToken m_metaModule[MAX_COMPONENTS] = {};
        uint32_t m_nextModuleId = 1;         // module ids; issued under m_registrationMutex
        std::deque<std::string> m_moduleNames;  // index = moduleId - 1; diagnostics

        // Sparse shadow stack: only ids that have EVER been overridden by a
        // different owner get an entry here. Written only under
        // m_registrationMutex (InstallOwned pushes; ReleaseModule pops/strips).
        FlatMap<ComponentID, SmallVector<ShadowEntry, 1>> m_shadow;
    };
}