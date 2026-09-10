#pragma once

#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../Component/Component.hpp"
#include "../Container/FlatMap.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/ModuleIdentity.hpp"
#include "../Core/TypeContext.hpp"
#include "../Core/TypeID.hpp"
#include "TypeMeta.hpp"

namespace Astra
{
    // Who is bound to a meta entry (spec 2026-09-09 §3.2). One per module
    // that has registered, adopted, or drained this type. The TOP binder is
    // the one whose closures the entry's TypeMeta content currently carries.
    struct MetaBinder
    {
        ModuleToken module = nullptr;
        MetaBuildFn build  = nullptr;   // null: this module has no reflect factory for the type
                                        // (registered a component it did not reflect) -- it can
                                        // hold refs but never source content (§3.6)
        uint32_t    refs   = 0;         // live-or-shadowed registry slots + RegisterMeta adoptions
        bool        pinned = false;     // resident module: never removed at zero refs
    };

    enum class BindOutcome    : uint8_t { Refused, Bound, BoundNeedsRebuild };
    enum class ReleaseOutcome : uint8_t { Unbound, Held, Dropped, Retained, Rebound, Erased };
    struct BindResult { BindOutcome outcome; TypeMeta* meta; };

    /**
     * Registry for type metadata, owned by a TypeContext.
     * Provides thread-safe registration and lookup of TypeMeta instances.
     * Instance() resolves through the active TypeContext, so all modules
     * sharing one context see the same metadata.
     *
     * Lifetime (spec 2026-09-09 §3.3): an entry's TypeMeta address is stable
     * for the entry's whole life. Registrations bind and acquire refs; a
     * binder leaves the stack only at zero refs and unpinned; the entry is
     * erased only when its stack is empty. Bind/Acquire/InstallBaseline never
     * run user code and may be called under a registry lock (lock order:
     * registration mutex -> meta mutex). Release and Rebind may run a
     * MetaBuildFn thunk -- user reflection code -- and must be called with
     * no registry lock held; the thunk itself runs with THIS mutex released.
     */
    class MetaRegistry
    {
    public:
        MetaRegistry() = default;
        ~MetaRegistry() = default;

        /**
         * Gets the registry of the active TypeContext.
         * Drains any pending static registrations first, so reflection
         * registered during static initialization is visible here.
         */
        static MetaRegistry& Instance()
        {
            TypeContext* ctx = GetTypeContext();
            Detail::DrainPendingMeta(*ctx);
            return ctx->Meta();
        }

        // Deleted copy/move
        MetaRegistry(const MetaRegistry&) = delete;
        MetaRegistry& operator=(const MetaRegistry&) = delete;
        MetaRegistry(MetaRegistry&&) = delete;
        MetaRegistry& operator=(MetaRegistry&&) = delete;

        /**
         * Registers a new type or returns the existing registration.
         * Thread-safe. Installs a binder-less entry: nothing holds it, and it
         * is erased only if a later Bind attaches a binder that then releases
         * to an empty stack -- the raw manual path, mostly for tests.
         * @param hash Type hash
         * @param name Type name
         * @return Reference to the TypeMeta (new or existing)
         */
        TypeMeta& Register(uint64_t hash, std::string_view name)
        {
            std::unique_lock lock(m_mutex);

            auto it = m_types.Find(hash);
            if (it != m_types.end())
            {
                // Hash hit: only the name is available to disambiguate here.
                // A DIFFERENT name on the same hash is a real XXHash64
                // collision of two distinct types -- refuse loudly rather than
                // alias the second type onto the first's metadata. Same name =>
                // idempotent re-registration, return the existing entry.
                // (Reference return type: cannot signal refusal via null, so
                // the loud log/ensure IS the refusal; mirrors TypeContext.)
                if (it->second.meta->typeName != name)
                {
                    std::string msg = "MetaRegistry: type-identity collision -- incoming type '";
                    msg.append(name);
                    msg += "' shares the type-hash of already-registered '";
                    msg.append(it->second.meta->typeName);
                    msg += "'. The second type is refused. Give types a unique unqualified name.";
                    ASTRA_LOG_ERROR(msg);
                    ASTRA_ENSURE_ALWAYS(false, "MetaRegistry type-identity collision");
                }
                return *it->second.meta;
            }

            TypeMeta fresh;
            fresh.typeHash = hash;
            fresh.typeName = name;
            return *InstallEntryLocked(hash, std::move(fresh)).meta;
        }

        /**
         * Registers a TypeMeta instance directly (binder-less, see above).
         * Thread-safe.
         * @param meta The TypeMeta to register (moved)
         * @return Pointer to the registered TypeMeta, nullptr on identity collision
         */
        TypeMeta* Register(TypeMeta&& meta)
        {
            std::unique_lock lock(m_mutex);

            uint64_t hash = meta.typeHash;
            auto it = m_types.Find(hash);
            if (it != m_types.end())
            {
                // Hash hit: compare the identity fields the TypeMeta already
                // carries. Matching size/alignment/triviality/name => the same
                // type re-registered (idempotent), return the existing entry.
                // Any difference is a real type-hash collision of two distinct
                // types -- refuse loudly and return nullptr rather than alias
                // one onto the other (aliasing mismatched size/alignment onto
                // an existing entry corrupts memory downstream).
                if (!SameIdentity(*it->second.meta, meta))
                {
                    RefuseCollision(*it->second.meta, meta);
                    return nullptr;
                }
                return it->second.meta.get();
            }
            return InstallEntryLocked(hash, std::move(meta)).meta.get();
        }

        // Raw content swap: installs `fresh` if the hash is unknown (binder-
        // less); on a hash hit with MATCHING identity move-assigns the
        // contents through the existing pointer -- the address every
        // ComponentDescriptor::meta caches stays valid, only the closures/
        // fields swap. Identity mismatch refuses (nullptr). Ignores the
        // binder stack entirely: prefer Bind/Rebind, which respect it.
        TypeMeta* RebindInPlace(TypeMeta&& fresh)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(fresh.typeHash);
            if (it == m_types.end())
            {
                uint64_t hash = fresh.typeHash;
                return InstallEntryLocked(hash, std::move(fresh)).meta.get();
            }
            TypeMeta& existing = *it->second.meta;
            if (!SameIdentity(existing, fresh))
            {
                ASTRA_LOG_ERROR("MetaRegistry: RebindInPlace identity mismatch -- refused");
                ASTRA_ENSURE_ALWAYS(false, "MetaRegistry rebind identity collision");
                return nullptr;
            }
            existing = std::move(fresh);   // move-assign: unique_ptr target address unchanged
            return &existing;
        }

        // ==== Binder stack (spec 2026-09-09 §3.3) =============================

        // Drain / ReflectType path. Entry absent: install from `fresh` with
        // `who`'s binder (refs 0, pinned iff Resident) as the only one. Entry
        // present: identity-check `fresh`, then, if `who` has no binder yet,
        // push one -- normally AT THE BOTTOM (first-wins content, no swap),
        // UNLESS every existing binder is null-build (spec 2026-09-09 §3.6
        // degraded state: the stack survived a departed top with nothing left
        // that can rebuild its content). In that case first-wins would strand
        // the entry with unrebuildable content forever, so this binder goes
        // ON TOP instead and the content is swapped to `fresh`, rescuing the
        // entry. A no-op if `who` already has a binder either way. Never runs
        // a thunk.
        BindResult InstallBaseline(uint64_t hash, Detail::ModuleIdentity who, MetaBuildFn build, TypeMeta&& fresh)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end())
            {
                Entry& e = InstallEntryLocked(hash, std::move(fresh));
                e.binders.push_back(MakeBinder(who, build));
                return { BindOutcome::Bound, e.meta.get() };
            }
            Entry& e = it->second;
            if (!SameIdentity(*e.meta, fresh))
            {
                RefuseCollision(*e.meta, fresh);
                return { BindOutcome::Refused, nullptr };
            }
            if (FindBinder(e, who.token) == e.binders.size())
            {
                if (AllBindersNullBuild(e))
                {
                    e.binders.push_back(MakeBinder(who, build));
                    *e.meta = std::move(fresh);
                }
                else
                {
                    e.binders.insert(e.binders.begin(), MakeBinder(who, build));
                }
            }
            return { BindOutcome::Bound, e.meta.get() };
        }

        // Registration path. Entry absent: install from `fresh` (Refused when
        // fresh is null) with `who`'s binder as the only one. Entry present,
        // no binder for `who` yet: push a NEW binder on top (build != null)
        // or just below the top (build == null -- it can hold refs but never
        // source content); if it landed on top, swap content to `fresh`, or,
        // when fresh is null and build != null, report BoundNeedsRebuild so
        // the caller runs build() OUTSIDE its locks and calls Rebind. Entry
        // present, binder exists: content is swapped only if that binder is
        // top AND fresh is given (same-image re-bind / in-binary reload);
        // otherwise nothing changes -- an existing top binder already carries
        // this module's closures. Identity mismatch on any supplied fresh ->
        // Refused. No ref change on any path. Never runs a thunk.
        BindResult Bind(uint64_t hash, Detail::ModuleIdentity who, MetaBuildFn build, TypeMeta* fresh)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end())
            {
                if (!fresh)
                {
                    return { BindOutcome::Refused, nullptr };   // nothing to install from
                }
                Entry& e = InstallEntryLocked(hash, std::move(*fresh));
                e.binders.push_back(MakeBinder(who, build));
                return { BindOutcome::Bound, e.meta.get() };
            }
            Entry& e = it->second;
            if (fresh && !SameIdentity(*e.meta, *fresh))
            {
                RefuseCollision(*e.meta, *fresh);
                return { BindOutcome::Refused, nullptr };
            }
            const size_t idx = FindBinder(e, who.token);
            if (idx == e.binders.size())
            {
                const bool onTop = (build != nullptr) || e.binders.empty();
                if (!onTop)
                {
                    e.binders.insert(e.binders.end() - 1, MakeBinder(who, build));
                    return { BindOutcome::Bound, e.meta.get() };
                }
                e.binders.push_back(MakeBinder(who, build));
                if (fresh)
                {
                    *e.meta = std::move(*fresh);
                    return { BindOutcome::Bound, e.meta.get() };
                }
                return { build ? BindOutcome::BoundNeedsRebuild : BindOutcome::Bound, e.meta.get() };
            }
            if (fresh && idx == e.binders.size() - 1)
            {
                *e.meta = std::move(*fresh);
            }
            return { BindOutcome::Bound, e.meta.get() };
        }

        // refs++ on an EXISTING binder. A caller must Bind before Acquire;
        // an absent binder is refused (ENSURE) and returns false.
        bool Acquire(uint64_t hash, ModuleToken module)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            const size_t idx = (it != m_types.end()) ? FindBinder(it->second, module) : 0;
            const bool bound = (it != m_types.end()) && idx < it->second.binders.size();
            if (!ASTRA_ENSURE_ALWAYS(bound, "MetaRegistry::Acquire without a prior Bind for this module"))
            {
                return false;
            }
            ++it->second.binders[idx].refs;
            return true;
        }

        // refs--. Held while refs remain. At zero: pinned -> Retained (binder
        // stays, nothing else changes); unpinned -> the binder is removed --
        // Dropped if it was not top; if it WAS top and other binders remain,
        // content is rebuilt from the new top's thunk OUTSIDE this mutex,
        // then swapped under it only if that binder is still top (Rebound).
        // Rebound means the rebind was ATTEMPTED from the new top: if the top
        // changed during the thunk window (a thunk may re-enter Bind), that
        // newer binder owns the content and the attempted build is dropped --
        // still reported Rebound, because the departing binder's release
        // completed. A new top with no build thunk cannot rebuild -> Retained
        // + warning (§3.6). An empty stack erases the entry and both link rows
        // (Erased). Unknown hash/module, or a binder already at zero refs,
        // -> Unbound (the latter ENSUREs: release without acquire).
        ReleaseOutcome Release(uint64_t hash, ModuleToken module)
        {
            MetaBuildFn rebuild = nullptr;
            ModuleToken newTop  = nullptr;
            {
                std::unique_lock lock(m_mutex);
                auto it = m_types.Find(hash);
                if (it == m_types.end())
                {
                    return ReleaseOutcome::Unbound;
                }
                Entry& e = it->second;
                const size_t idx = FindBinder(e, module);
                if (idx == e.binders.size())
                {
                    return ReleaseOutcome::Unbound;
                }
                MetaBinder& b = e.binders[idx];
                if (!ASTRA_ENSURE_ALWAYS(b.refs > 0, "MetaRegistry::Release on a binder with no refs (release without acquire)"))
                {
                    return ReleaseOutcome::Unbound;
                }
                --b.refs;
                if (b.refs > 0)
                {
                    return ReleaseOutcome::Held;
                }
                if (b.pinned)
                {
                    return ReleaseOutcome::Retained;
                }
                const bool wasTop = (idx == e.binders.size() - 1);
                e.binders.erase(e.binders.begin() + idx);
                if (e.binders.empty())
                {
                    EraseEntryLocked(it);
                    return ReleaseOutcome::Erased;
                }
                if (!wasTop)
                {
                    return ReleaseOutcome::Dropped;
                }
                const MetaBinder& top = e.binders.back();
                if (top.build == nullptr)
                {
                    // Only binders without a factory remain: the content still
                    // points at the departed module's closures and nothing can
                    // rebuild it. Keep the entry (its address stays valid for
                    // cached descriptors); name the hash, NOT typeName, which
                    // may already view unmapped memory (§3.6).
                    ASTRA_LOG_WARN("MetaRegistry: meta content for hash " + std::to_string(hash)
                                   + " is bound to a departed module and no remaining binder can rebuild it");
                    return ReleaseOutcome::Retained;
                }
                rebuild = top.build;
                newTop  = top.module;
            }

            // Outside the mutex: user reflection code.
            TypeMeta fresh = rebuild();

            {
                std::unique_lock lock(m_mutex);
                auto it = m_types.Find(hash);
                if (it != m_types.end() && !it->second.binders.empty()
                    && it->second.binders.back().module == newTop
                    && SameIdentity(*it->second.meta, fresh))
                {
                    *it->second.meta = std::move(fresh);
                }
                // else: the top changed while the thunk ran (a re-entrant
                // Bind) -- that binder owns the content now; drop ours. The
                // token compare is NOT ABA-proof against an erase-and-reinstall
                // at this hash during the window, and need not be: `fresh`
                // came from that module's own thunk and is identity-checked,
                // so swapping it into a reinstalled entry of the same identity
                // is still correct.
            }
            return ReleaseOutcome::Rebound;
        }

        // Content refresh for a module whose binder is (still) top: the
        // deferred half of BoundNeedsRebuild, run by the caller after it has
        // dropped its own locks. Identity-checked. Returns false (no change)
        // when the module's binder is not top or the entry is gone.
        bool Rebind(uint64_t hash, ModuleToken module, TypeMeta&& fresh)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end() || it->second.binders.empty())
            {
                return false;
            }
            if (it->second.binders.back().module != module)
            {
                return false;
            }
            if (!SameIdentity(*it->second.meta, fresh))
            {
                RefuseCollision(*it->second.meta, fresh);
                return false;
            }
            *it->second.meta = std::move(fresh);
            return true;
        }

        // ---- Diagnostics (tests, tools) ----
        ASTRA_NODISCARD size_t BinderCount(uint64_t hash) const
        {
            std::shared_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            return it != m_types.end() ? it->second.binders.size() : 0;
        }

        ASTRA_NODISCARD uint32_t Refs(uint64_t hash, ModuleToken module) const
        {
            std::shared_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end()) return 0;
            const size_t idx = FindBinder(it->second, module);
            return idx < it->second.binders.size() ? it->second.binders[idx].refs : 0;
        }

        ASTRA_NODISCARD bool IsPinned(uint64_t hash, ModuleToken module) const
        {
            std::shared_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end()) return false;
            const size_t idx = FindBinder(it->second, module);
            return idx < it->second.binders.size() && it->second.binders[idx].pinned;
        }

        ASTRA_NODISCARD ModuleToken TopBinder(uint64_t hash) const
        {
            std::shared_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            return (it != m_types.end() && !it->second.binders.empty()) ? it->second.binders.back().module : nullptr;
        }

        // Public erase: for module-owned NON-component metas (RegisterMeta
        // teardown). Refuses a component-linked hash -- erasing one would
        // dangle every cached ComponentDescriptor::meta for a live component.
        // TRANSITIONAL: deleted in the next task once ComponentModule releases
        // through the binder stack instead.
        bool Erase(uint64_t hash)
        {
            std::unique_lock lock(m_mutex);
            if (m_typeToComponentId.Find(hash) != m_typeToComponentId.end())
            {
                ASTRA_ENSURE_ALWAYS(false,
                    "MetaRegistry::Erase refused: hash is component-linked (use the "
                    "component clear path, not a manual erase)");
                return false;
            }
            return m_types.Erase(hash) != 0;
        }

        // TRANSITIONAL clear-path erase (deleted in the next task with its
        // last caller): removes the entry AND both link rows.
        bool EraseUnchecked(uint64_t hash)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end())
            {
                return false;
            }
            EraseEntryLocked(it);
            return true;
        }

        /**
         * Gets type metadata by hash.
         * Thread-safe.
         * @param hash Type hash
         * @return Pointer to TypeMeta, or nullptr if not registered
         */
        ASTRA_NODISCARD const TypeMeta* Get(uint64_t hash) const
        {
            std::shared_lock lock(m_mutex);

            auto it = m_types.Find(hash);
            if (it != m_types.end())
            {
                return it->second.meta.get();
            }
            return nullptr;
        }

        /**
         * Gets type metadata by type.
         * Thread-safe.
         * @tparam T The type to look up
         * @return Pointer to TypeMeta, or nullptr if not registered
         */
        template<typename T>
        ASTRA_NODISCARD const TypeMeta* Get() const
        {
            return Get(TypeID<T>::Hash());
        }

        /**
         * Gets type metadata by name.
         * Thread-safe but slower than hash lookup.
         * @param name Type name
         * @return Pointer to TypeMeta, or nullptr if not registered
         */
        ASTRA_NODISCARD const TypeMeta* GetByName(std::string_view name) const
        {
            std::shared_lock lock(m_mutex);

            for (const auto& [hash, entry] : m_types)
            {
                if (entry.meta->typeName == name)
                {
                    return entry.meta.get();
                }
            }
            return nullptr;
        }

        /**
         * Checks if a type is registered.
         * Thread-safe.
         * @param hash Type hash
         * @return true if registered
         */
        ASTRA_NODISCARD bool IsRegistered(uint64_t hash) const
        {
            std::shared_lock lock(m_mutex);
            return m_types.Find(hash) != m_types.end();
        }

        /**
         * Checks if a type is registered.
         * Thread-safe.
         * @tparam T The type to check
         * @return true if registered
         */
        template<typename T>
        ASTRA_NODISCARD bool IsRegistered() const
        {
            return IsRegistered(TypeID<T>::Hash());
        }

        /**
         * Links a type hash to a ComponentID for ECS integration.
         * Thread-safe. Link rows are context facts (ComponentIDs are
         * context-scoped) and are dropped only when the entry is erased.
         * @param typeHash Type hash
         * @param componentId ComponentID from ComponentRegistry
         */
        void LinkToComponent(uint64_t typeHash, ComponentID componentId)
        {
            std::unique_lock lock(m_mutex);
            m_typeToComponentId[typeHash] = componentId;
            m_componentIdToType[componentId] = typeHash;
        }

        /**
         * Gets the ComponentID for a type hash.
         * Thread-safe.
         * @param typeHash Type hash
         * @return ComponentID, or INVALID_COMPONENT if not linked
         */
        ASTRA_NODISCARD ComponentID GetComponentId(uint64_t typeHash) const
        {
            std::shared_lock lock(m_mutex);

            auto it = m_typeToComponentId.Find(typeHash);
            if (it != m_typeToComponentId.end())
            {
                return it->second;
            }
            return INVALID_COMPONENT;
        }

        /**
         * Gets the type hash for a ComponentID.
         * Thread-safe.
         * @param componentId ComponentID
         * @return Type hash, or 0 if not linked
         */
        ASTRA_NODISCARD uint64_t GetTypeHash(ComponentID componentId) const
        {
            std::shared_lock lock(m_mutex);

            auto it = m_componentIdToType.Find(componentId);
            if (it != m_componentIdToType.end())
            {
                return it->second;
            }
            return 0;
        }

        /**
         * Gets type metadata by ComponentID.
         * Thread-safe.
         * @param componentId ComponentID from ComponentRegistry
         * @return Pointer to TypeMeta, or nullptr if not linked
         */
        ASTRA_NODISCARD const TypeMeta* GetByComponentId(ComponentID componentId) const
        {
            uint64_t hash = GetTypeHash(componentId);
            if (hash == 0)
            {
                return nullptr;
            }
            return Get(hash);
        }

        /**
         * Gets the number of registered types.
         * Thread-safe.
         */
        ASTRA_NODISCARD size_t GetRegisteredCount() const
        {
            std::shared_lock lock(m_mutex);
            return m_types.Size();
        }

        /**
         * Iterates over all registered types.
         * Thread-safe. Snapshots the entries under the lock, then releases it
         * before invoking the callback: std::shared_mutex is NOT recursive, so
         * a callback that re-enters any read accessor (Get/GetByName/...) while
         * the shared lock was still held would be UB/deadlock.
         * @tparam Func Callback type
         * @param func Callback invoked for each TypeMeta
         */
        template<typename Func>
        void ForEachType(Func&& func) const
        {
            std::vector<TypeMeta*> snapshot;
            {
                std::shared_lock lock(m_mutex);
                snapshot.reserve(m_types.Size());
                for (const auto& [hash, entry] : m_types)
                {
                    snapshot.push_back(entry.meta.get());
                }
            }

            for (TypeMeta* meta : snapshot)
            {
                func(*meta);
            }
        }

        /**
         * Clears all registrations (entries, binders, link rows).
         * Thread-safe. Use with caution - mainly for testing.
         */
        void Clear()
        {
            std::unique_lock lock(m_mutex);
            m_types.Clear();
            m_typeToComponentId.Clear();
            m_componentIdToType.Clear();
        }

    private:
        struct Entry
        {
            std::unique_ptr<TypeMeta>  meta;      // address-stable for the entry's life
            SmallVector<MetaBinder, 2> binders;   // back() == top == whose closures `meta` carries
        };

        // All helpers below require m_mutex to be held (unique for mutators).

        Entry& InstallEntryLocked(uint64_t hash, TypeMeta&& fresh)
        {
            Entry& e = m_types[hash];
            e.meta = std::make_unique<TypeMeta>(std::move(fresh));
            return e;
        }

        void EraseEntryLocked(typename FlatMap<uint64_t, Entry>::iterator it)
        {
            const uint64_t hash = it->first;
            if (auto link = m_typeToComponentId.Find(hash); link != m_typeToComponentId.end())
            {
                m_componentIdToType.Erase(link->second);
                m_typeToComponentId.Erase(link);
            }
            m_types.Erase(it);
        }

        static MetaBinder MakeBinder(Detail::ModuleIdentity who, MetaBuildFn build) noexcept
        {
            return MetaBinder{ who.token, build, 0u, who.residency == ModuleResidency::Resident };
        }

        // Index of `module`'s binder in e.binders, or e.binders.size() if none.
        static size_t FindBinder(const Entry& e, ModuleToken module) noexcept
        {
            for (size_t i = 0; i < e.binders.size(); ++i)
            {
                if (e.binders[i].module == module) return i;
            }
            return e.binders.size();
        }

        // True when no binder in the stack can source content (spec
        // 2026-09-09 §3.6 degraded state) -- vacuously true for an empty
        // stack, which never occurs on InstallBaseline's existing-entry path.
        static bool AllBindersNullBuild(const Entry& e) noexcept
        {
            for (const MetaBinder& b : e.binders)
            {
                if (b.build != nullptr) return false;
            }
            return true;
        }

        // The identity fields a TypeMeta carries: matching => the same type
        // (re-registered / rebuilt), anything else => a real type-hash
        // collision of two distinct types.
        static bool SameIdentity(const TypeMeta& a, const TypeMeta& b) noexcept
        {
            return a.size == b.size
                && a.alignment == b.alignment
                && a.isTrivial == b.isTrivial
                && a.typeName == b.typeName;
        }

        static void RefuseCollision(const TypeMeta& existing, const TypeMeta& incoming)
        {
            std::string msg = "MetaRegistry: type-identity collision -- incoming type '";
            msg.append(incoming.typeName);
            msg += "' shares the type-hash of already-registered '";
            msg.append(existing.typeName);
            msg += "'. The second type is refused (nullptr). Give types a unique unqualified name.";
            ASTRA_LOG_ERROR(msg);
            ASTRA_ENSURE_ALWAYS(false, "MetaRegistry type-identity collision");
        }

        mutable std::shared_mutex m_mutex;
        FlatMap<uint64_t, Entry> m_types;
        FlatMap<uint64_t, ComponentID> m_typeToComponentId;
        FlatMap<ComponentID, uint64_t> m_componentIdToType;
    };

    // Declared in TypeContext.hpp (where MetaRegistry is forward-declared);
    // defined here, where MetaRegistry is complete.
    inline MetaRegistry& TypeContext::Meta()
    {
        std::lock_guard lock(m_metaMutex);
        if (!m_meta)
        {
            m_meta = std::make_shared<MetaRegistry>();
        }
        return *m_meta;
    }

    // ============================================================================
    // Convenience functions
    // ============================================================================

    /**
     * Gets type metadata for a type.
     * @tparam T The type to look up
     * @return Pointer to TypeMeta, or nullptr if not registered
     */
    template<typename T>
    inline const TypeMeta* GetMeta()
    {
        return MetaRegistry::Instance().Get<T>();
    }

    /**
     * Gets type metadata by hash.
     * @param hash Type hash
     * @return Pointer to TypeMeta, or nullptr if not registered
     */
    inline const TypeMeta* GetMeta(uint64_t hash)
    {
        return MetaRegistry::Instance().Get(hash);
    }

    /**
     * Gets type metadata by name.
     * @param name Type name
     * @return Pointer to TypeMeta, or nullptr if not registered
     */
    inline const TypeMeta* GetMetaByName(std::string_view name)
    {
        return MetaRegistry::Instance().GetByName(name);
    }

    /**
     * Gets type metadata by ComponentID.
     * @param componentId ComponentID from ComponentRegistry
     * @return Pointer to TypeMeta, or nullptr if not linked
     */
    inline const TypeMeta* GetMeta(ComponentID componentId)
    {
        return MetaRegistry::Instance().GetByComponentId(componentId);
    }

    /**
     * Checks if a type is registered for reflection.
     * @tparam T The type to check
     * @return true if registered
     */
    template<typename T>
    inline bool IsReflected()
    {
        return MetaRegistry::Instance().IsRegistered<T>();
    }

    namespace Detail
    {
        // Per-T-per-module meta factory, retained so a module handle can
        // REBUILD this module's TypeMeta later (hot-reload rebind). Inline
        // template static: each DLL/EXE gets its own copy, which is exactly
        // the ownership boundary the rebuild must respect.
        //
        // `fn` is a trivially-constructed proxy, NOT a bare std::function.
        // MetaFactory<T>::fn and the ASTRA_REFLECT_TYPE registrar variable
        // that assigns to it are BOTH inline/template statics, so both have
        // "unordered" dynamic initialization ([basic.start.dynamic]) with no
        // ordering guarantee between them -- empirically confirmed on MSVC
        // (repro'd in isolation and reproduced with this exact header): a
        // plain `static inline std::function<TypeMeta()> fn;` member can
        // have its own (empty) default-construction scheduled AFTER the
        // registrar's constructor assigns to it, silently discarding the
        // assignment (fn reads back empty/throws bad_function_call).
        // Routing the real storage through a function-local static
        // sidesteps "unordered init" entirely: the proxy itself is an empty
        // type needing no dynamic initialization, and the actual
        // std::function is constructed exactly once, on first touch,
        // per the language's construct-on-first-use guarantee for
        // function-local statics (thread-safe, no ordering assumptions).
        // Proxy mirrors std::function's surface: assignment, explicit bool,
        // call, and implicit conversion to std::function<TypeMeta()>.
        template<typename T>
        struct MetaFactory
        {
        private:
            static std::function<TypeMeta()>& Storage()
            {
                static std::function<TypeMeta()> s;
                return s;
            }

        public:
            struct FactorySlot
            {
                FactorySlot& operator=(std::function<TypeMeta()> f)
                {
                    Storage() = std::move(f);
                    return *this;
                }
                explicit operator bool() const { return static_cast<bool>(Storage()); }
                TypeMeta operator()() const { return Storage()(); }
                operator std::function<TypeMeta()>() const { return Storage(); }
            };

            static inline FactorySlot fn;
        };

        // Adapts MetaFactory<T>::fn (a FactorySlot proxy) to the plain
        // `TypeMeta (*)()` shape of MetaBuildFn, so an owned component slot can
        // retain a rebuild callback without ComponentRegistry/ComponentDescriptor
        // needing to know about T. Caller must check MetaFactory<T>::fn's
        // truthiness before taking this function's address -- calling through it
        // when fn is empty throws (see FactorySlot::operator()).
        template<typename T>
        TypeMeta BuildMetaThunk()
        {
            return MetaFactory<T>::fn();
        }

        /**
         * Helper struct for static registration of types.
         * Used by ASTRA_REFLECT_TYPE macro.
         *
         * Runs at static initialization, BEFORE a host can install a shared
         * TypeContext, so it builds the metadata eagerly but DEFERS the
         * registration onto the module-local pending queue. The queue drains
         * into the installed context on SetTypeContext(), or into the
         * module-default context on first MetaRegistry::Instance() access.
         */
        template<typename T>
        struct StaticTypeRegistrar
        {
            template<typename BuilderFunc>
            explicit StaticTypeRegistrar(BuilderFunc&& builderFunc)
            {
                MetaFactory<T>::fn = [f = builderFunc]() {
                    TypeMetaBuilder<T> b;
                    f(b);
                    return b.Build();
                };

                TypeMetaBuilder<T> builder;
                builderFunc(builder);
                // TypeMeta is move-only; hold it via shared_ptr so the
                // deferred registration stays copyable for std::function.
                auto meta = std::make_shared<TypeMeta>(builder.Build());
                EnqueuePendingMeta([meta](TypeContext& ctx)
                {
                    ctx.Meta().Register(std::move(*meta));
                });
            }
        };
    }
}
