#pragma once

#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../Component/Component.hpp"
#include "../Container/FlatMap.hpp"
#include "../Core/TypeContext.hpp"
#include "../Core/TypeID.hpp"
#include "TypeMeta.hpp"

namespace Astra
{
    /**
     * Registry for type metadata, owned by a TypeContext.
     * Provides thread-safe registration and lookup of TypeMeta instances.
     * Instance() resolves through the active TypeContext, so all modules
     * sharing one context see the same metadata.
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
         * Thread-safe.
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
                if (it->second->typeName != name)
                {
                    std::string msg = "MetaRegistry: type-identity collision -- incoming type '";
                    msg.append(name);
                    msg += "' shares the type-hash of already-registered '";
                    msg.append(it->second->typeName);
                    msg += "'. The second type is refused. Give types a unique unqualified name.";
                    ASTRA_LOG_ERROR(msg);
                    ASTRA_ENSURE_ALWAYS(false, "MetaRegistry type-identity collision");
                }
                return *it->second;
            }

            auto meta = std::make_unique<TypeMeta>();
            meta->typeHash = hash;
            meta->typeName = name;
            TypeMeta* ptr = meta.get();
            m_types[hash] = std::move(meta);
            return *ptr;
        }

        /**
         * Registers a TypeMeta instance directly.
         * Thread-safe.
         * @param meta The TypeMeta to register (moved)
         * @return Pointer to the registered TypeMeta
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
                const TypeMeta& existing = *it->second;
                if (existing.size != meta.size
                    || existing.alignment != meta.alignment
                    || existing.isTrivial != meta.isTrivial
                    || existing.typeName != meta.typeName)
                {
                    std::string msg = "MetaRegistry: type-identity collision -- incoming type '";
                    msg.append(meta.typeName);
                    msg += "' shares the type-hash of already-registered '";
                    msg.append(existing.typeName);
                    msg += "'. The second type is refused (nullptr). Give types a unique unqualified name.";
                    ASTRA_LOG_ERROR(msg);
                    ASTRA_ENSURE_ALWAYS(false, "MetaRegistry type-identity collision");
                    return nullptr;
                }
                // Idempotent re-registration - return existing
                return it->second.get();
            }

            auto metaPtr = std::make_unique<TypeMeta>(std::move(meta));
            TypeMeta* ptr = metaPtr.get();
            m_types[hash] = std::move(metaPtr);
            return ptr;
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
                return it->second.get();
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

            for (const auto& [hash, meta] : m_types)
            {
                if (meta->typeName == name)
                {
                    return meta.get();
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
         * Thread-safe.
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
                for (const auto& [hash, meta] : m_types)
                {
                    snapshot.push_back(meta.get());
                }
            }

            for (TypeMeta* meta : snapshot)
            {
                func(*meta);
            }
        }

        /**
         * Clears all registrations.
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
        mutable std::shared_mutex m_mutex;
        FlatMap<uint64_t, std::unique_ptr<TypeMeta>> m_types;
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
