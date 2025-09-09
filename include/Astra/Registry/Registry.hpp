#pragma once

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

#include "../Archetype/Archetype.hpp"
#include "../Archetype/ArchetypeManager.hpp"
#include "../Component/Component.hpp"
#include "../Container/SmallVector.hpp"
#include "../Component/ComponentRegistry.hpp"
#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Core/Signal.hpp"
#include "../Core/TypeID.hpp"
#include "../Entity/Entity.hpp"
#include "../Entity/EntityManager.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "../Serialization/SerializationError.hpp"
#include "Query.hpp"
#include "Relations.hpp"
#include "RelationshipGraph.hpp"
#include "View.hpp"

namespace Astra
{
    class Registry
    {
    public:
        struct Config
        {
            EntityManager::Config entityManagerConfig{};  // GCC fix: explicit default initialization
            ArchetypeChunkPool::Config chunkPoolConfig{}; // GCC fix: explicit default initialization
            
            // GCC fix: explicit default constructor for brace initialization
            Config() = default;
        };
        
    // Original: explicit Registry(const Config& config = {})
    // GCC fix: Provide both explicit config constructor and default constructor
    explicit Registry() : Registry(Config()) {}
    
    explicit Registry(Config config) :
            m_entityManager(std::make_shared<EntityManager>(config.entityManagerConfig)),
            m_archetypeManager(std::make_shared<ArchetypeManager>(config.chunkPoolConfig))
        {}
        
        Registry(const EntityManager::Config& entityConfig, const ArchetypeChunkPool::Config& chunkConfig) :
            m_entityManager(std::make_shared<EntityManager>(entityConfig)),
            m_archetypeManager(std::make_shared<ArchetypeManager>(chunkConfig))
        {}
        
    // GCC fix: Provide overloaded constructors to avoid default member initializer issues
    Registry(std::shared_ptr<ComponentRegistry> componentRegistry) : Registry(componentRegistry, Config()) {}
    
    Registry(std::shared_ptr<ComponentRegistry> componentRegistry, Config config) :
            m_entityManager(std::make_shared<EntityManager>(config.entityManagerConfig)),
            m_archetypeManager(std::make_shared<ArchetypeManager>(componentRegistry, config.chunkPoolConfig))
        {}
        
    // GCC fix: Provide overloaded constructors to avoid default member initializer issues
    explicit Registry(const Registry& other) : Registry(other, Config()) {}
    
    explicit Registry(const Registry& other, Config config) :
            m_entityManager(std::make_shared<EntityManager>(config.entityManagerConfig)),
            m_archetypeManager(std::make_shared<ArchetypeManager>(other.GetComponentRegistry(), config.chunkPoolConfig))
        {}
        
        template<Component... Components>
        Entity CreateEntity()
        {
            if constexpr (sizeof...(Components) == 0)
            {
                Entity entity = m_entityManager->Create();
                m_archetypeManager->AddEntity(entity);
                
                m_signalManager.Emit<Events::EntityCreated>(entity);
                
                return entity;
            }
            else
            {
                return CreateEntityWith(Components{}...);
            }
        }
        
        template<Component... Components>
        Entity CreateEntityWith(Components&&... components)
        {
            Entity entity = m_entityManager->Create();
            Archetype* archetype = m_archetypeManager->GetOrCreateArchetype<Components...>();
            EntityLocation location = archetype->AddEntity(entity);
            
            if (!location.IsValid())
            {
                m_entityManager->Destroy(entity);
                return Entity::Invalid();
            }
            
            ((archetype->SetComponent<Components>(location, std::forward<Components>(components))), ...);
            
            m_archetypeManager->SetEntityLocation(entity, archetype, location);
            
            m_signalManager.Emit<Events::EntityCreated>(entity);
            
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                ((m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<Components>::Value(), archetype->GetComponent<Components>(location))), ...);
            }
            
            return entity;
        }

        template<Component... Components>
        void CreateEntities(size_t count, std::span<Entity> outEntities)
        {
            if (count == 0 || outEntities.size() < count)
                return;
            
            m_entityManager->CreateBatch(count, outEntities.begin());
            
            if constexpr (sizeof...(Components) > 0)
            {
                auto generator = [](size_t) { return std::make_tuple(Components{}...); };
                m_archetypeManager->AddEntities<Components...>(outEntities.subspan(0, count), generator);
            }
            else
            {
                for (size_t i = 0; i < count; ++i)
                {
                    m_archetypeManager->AddEntity(outEntities[i]);
                }
            }
            
            if (m_signalManager.IsSignalEnabled(Signal::EntityCreated))
            {
                for (size_t i = 0; i < count; ++i)
                {
                    m_signalManager.Emit<Events::EntityCreated>(outEntities[i]);
                }
            }
            
            if constexpr (sizeof...(Components) > 0)
            {
                if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
                {
                    for (size_t i = 0; i < count; ++i)
                    {
                        ((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), nullptr)), ...);
                    }
                }
            }
        }

        template<Component... Components, typename Generator>
        void CreateEntitiesWith(size_t count, std::span<Entity> outEntities, Generator&& generator)
        {
            if (count == 0 || outEntities.size() < count)
                return;
            
            m_entityManager->CreateBatch(count, outEntities.begin());
            m_archetypeManager->AddEntities<Components...>(outEntities.subspan(0, count), std::forward<Generator>(generator));
            
            if (m_signalManager.IsSignalEnabled(Signal::EntityCreated))
            {
                for (size_t i = 0; i < count; ++i)
                {
                    m_signalManager.Emit<Events::EntityCreated>(outEntities[i]);
                }
            }
            
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                for (size_t i = 0; i < count; ++i)
                {
                    ((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), nullptr)), ...);
                }
            }
        }

        void DestroyEntity(Entity entity)
        {
            if (!m_entityManager->IsValid(entity))
                return;
            
            m_signalManager.Emit<Events::EntityDestroyed>(entity);
            
            m_archetypeManager->RemoveEntity(entity);
            m_relationshipGraph.OnEntityDestroyed(entity);
            m_entityManager->Destroy(entity);
        }
        
        void DestroyEntities(std::span<Entity> entities)
        {
            if (entities.empty())
                return;
            
            std::vector<Entity> validEntities;
            
            size_t reserveSize = std::min(entities.size(), size_t(10000));
            validEntities.reserve(reserveSize);
            
            for (Entity entity : entities)
            {
                if (m_entityManager->IsValid(entity))
                {
                    validEntities.push_back(entity);
                }
            }
            
            if (validEntities.empty())
                return;

            if (m_signalManager.IsSignalEnabled(Signal::EntityDestroyed))
            {
                for (Entity entity : validEntities)
                {
                    m_signalManager.Emit<Events::EntityDestroyed>(entity);
                }
            }
                
            m_archetypeManager->RemoveEntities(validEntities);
            
            for (Entity entity : validEntities)
            {
                m_relationshipGraph.OnEntityDestroyed(entity);
            }
            
            for (Entity entity : validEntities)
            {
                m_entityManager->Destroy(entity);
            }
        }

        ASTRA_NODISCARD bool IsValid(Entity entity) const noexcept
        {
            return m_entityManager->IsValid(entity);
        }

        template<Component T, typename... Args>
        void AddComponent(Entity entity, Args&&... args)
        {
            if (!m_entityManager->IsValid(entity))
                return;
                
            T* component = m_archetypeManager->AddComponent<T>(entity, std::forward<Args>(args)...);
            
            if (component)
            {
                m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), component);
            }
        }

        template<Component T>
        bool RemoveComponent(Entity entity)
        {
            if (!m_entityManager->IsValid(entity))
                return false;
            
            T* component = m_archetypeManager->GetComponent<T>(entity);
            bool removed = m_archetypeManager->RemoveComponent<T>(entity);
            
            if (removed && component)
            {
                m_signalManager.Emit<Events::ComponentRemoved>(entity, TypeID<T>::Value(), component);
            }
            
            return removed;
        }
        
        template<Component T, typename... Args>
        void AddComponents(std::span<Entity> entities, Args&&... args)
        {
            if (entities.empty())
                return;
            
            // Filter out invalid entities
            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(entities.size());
            
            for (Entity entity : entities)
            {
                if (m_entityManager->IsValid(entity))
                {
                    validEntities.push_back(entity);
                }
            }
            
            if (validEntities.empty())
                return;
            
            // Batch add components
            m_archetypeManager->AddComponents<T>(validEntities, std::forward<Args>(args)...);
            
            // Emit signals if enabled
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                for (Entity entity : validEntities)
                {
                    T* component = m_archetypeManager->GetComponent<T>(entity);
                    if (component)
                    {
                        m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), component);
                    }
                }
            }
        }
        
        template<Component T>
        size_t RemoveComponents(std::span<Entity> entities)
        {
            if (entities.empty())
                return 0;
            
            // Filter out invalid entities and collect components for signals
            SmallVector<Entity, 256> validEntities;
            SmallVector<T*, 256> componentsToRemove;
            validEntities.reserve(entities.size());
            
            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                componentsToRemove.reserve(entities.size());
                for (Entity entity : entities)
                {
                    if (m_entityManager->IsValid(entity))
                    {
                        T* component = m_archetypeManager->GetComponent<T>(entity);
                        if (component)
                        {
                            validEntities.push_back(entity);
                            componentsToRemove.push_back(component);
                        }
                    }
                }
            }
            else
            {
                for (Entity entity : entities)
                {
                    if (m_entityManager->IsValid(entity))
                    {
                        validEntities.push_back(entity);
                    }
                }
            }
            
            if (validEntities.empty())
                return 0;
            
            // Batch remove components
            size_t removedCount = m_archetypeManager->RemoveComponents<T>(validEntities);
            
            // Emit signals if enabled
            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                for (size_t i = 0; i < removedCount && i < componentsToRemove.size(); ++i)
                {
                    m_signalManager.Emit<Events::ComponentRemoved>(validEntities[i], TypeID<T>::Value(), componentsToRemove[i]);
                }
            }
            
            return removedCount;
        }

        template<Component T>
        ASTRA_NODISCARD T* GetComponent(Entity entity)
        {
            if (!m_entityManager->IsValid(entity))
                return nullptr;
            return m_archetypeManager->GetComponent<T>(entity);
        }
        
        template<Component T>
        ASTRA_NODISCARD const T* GetComponent(Entity entity) const
        {
            if (!m_entityManager->IsValid(entity))
                return nullptr;
            return m_archetypeManager->GetComponent<T>(entity);
        }
        
        template<Component T>
        ASTRA_NODISCARD bool HasComponent(Entity entity) const
        {
            if (!m_entityManager->IsValid(entity))
                return false;
            return m_archetypeManager->HasComponent<T>(entity);
        }

        template<ValidQueryArg... QueryArgs>
        ASTRA_NODISCARD auto CreateView()
        {
            return View<QueryArgs...>(m_archetypeManager);
        }
        
        void Clear()
        {
            if (m_signalManager.IsSignalEnabled(Signal::EntityDestroyed))
            {
                // TODO: Consider if we want to emit signals during Clear()
                // For now, we skip signals as Clear() is typically used for bulk cleanup
            }
            
            auto componentRegistry = m_archetypeManager->GetComponentRegistry();
            m_archetypeManager = std::make_shared<ArchetypeManager>(std::move(componentRegistry), ArchetypeChunkPool::Config{});

            m_relationshipGraph.Clear();
            
            m_entityManager->Clear();
            
            // Note: We don't clear signal handlers here as they may still be valid
            // for future entities. Users can manually clear handlers if needed.
        }

        ASTRA_NODISCARD std::size_t Size() const noexcept
        {
            return m_entityManager->Size();
        }

        ASTRA_NODISCARD bool IsEmpty() const noexcept
        {
            return Size() == 0;
        }
        
        ASTRA_NODISCARD const ArchetypeManager& GetArchetypeManager() const { return *m_archetypeManager; }
        ASTRA_NODISCARD ArchetypeManager& GetArchetypeManager() { return *m_archetypeManager; }

        std::shared_ptr<ComponentRegistry> GetComponentRegistry() const { return m_archetypeManager->GetComponentRegistry(); }
        
        // ====================== Archetype API ======================
        
        // Re-export types from ArchetypeManager for convenience
        using ArchetypeInfo = ArchetypeManager::ArchetypeInfo;
        
        /**
         * Options for controlling defragmentation behavior
         */
        // HOTFIX: Move DefragmentationOptions above its first use as a default parameter (GCC/Clang requirement)
        struct DefragmentationOptions
        {
            // Archetype-level cleanup
            // Original: size_t minEmptyDuration = 1;              // Remove archetypes empty for N updates
            size_t minEmptyDuration = 1;              // Remove archetypes empty for N updates
            // Original: size_t minArchetypesToKeep = 8;           // Never go below this many archetypes
            size_t minArchetypesToKeep = 8;           // Never go below this many archetypes
            // Original: size_t maxArchetypesToRemove = 10;        // Limit per call (for incremental)
            size_t maxArchetypesToRemove = 10;        // Limit per call (for incremental)
            // Chunk-level defragmentation
            // Original: bool defragmentChunks = true;             // Enable chunk coalescing
            bool defragmentChunks = true;             // Enable chunk coalescing
            // Original: float chunkUtilizationThreshold = 0.5f;   // Pack chunks below this utilization
            float chunkUtilizationThreshold = 0.5f;   // Pack chunks below this utilization
            // Original: size_t maxChunksToProcess = 100;          // Limit chunks processed per call
            size_t maxChunksToProcess = 100;          // Limit chunks processed per call
            // Global limits
            // Original: size_t maxEntitiesToMove = 10000;         // Total entity move budget
            size_t maxEntitiesToMove = 10000;         // Total entity move budget
            // Original: bool incremental = false;                 // If true, strictly respect all limits
            bool incremental = false;                 // If true, strictly respect all limits
            
            // GCC fix: explicit default constructor for brace initialization
            DefragmentationOptions() = default;
        };
        
        /**
         * Results from a defragmentation operation
         */
        struct DefragmentationResult
        {
            size_t archetypesRemoved = 0;
            size_t chunksRemoved = 0;
            size_t entitiesMoved = 0;
            size_t archetypesProcessed = 0;
            float fragmentationBefore = 0.0f;
            float fragmentationAfter = 0.0f;
            
            ASTRA_NODISCARD bool DidWork() const noexcept 
            { 
                return archetypesRemoved > 0 || chunksRemoved > 0 || entitiesMoved > 0; 
            }
        };
        
        /**
         * Unified defragmentation operation
         * 
         * Performs both chunk coalescing and empty archetype removal in a single call.
         * Can be configured for incremental operation during gameplay or aggressive
         * cleanup during loading screens.
         * 
         * @param options Configuration for defragmentation behavior
         * @return Result containing statistics about the operation
         * 
         * Example usage:
         *   // Simple full defragmentation
         *   auto result = registry.Defragment();
         *   
         *   // Incremental during gameplay (limit work per frame)
         *   DefragmentationOptions incremental;
         *   incremental.incremental = true;
         *   incremental.maxEntitiesToMove = 100;
         *   incremental.maxChunksToProcess = 10;
         *   auto result = registry.Defragment(incremental);
         *   
         *   // Aggressive during loading screen
         *   DefragmentationOptions aggressive;
         *   aggressive.maxArchetypesToRemove = SIZE_MAX;
         *   aggressive.maxEntitiesToMove = SIZE_MAX;
         *   aggressive.chunkUtilizationThreshold = 0.9f;
         *   auto result = registry.Defragment(aggressive);
         */
    // Original: DefragmentationResult Defragment(const DefragmentationOptions& options = {})
    // HOTFIX: DefragmentationOptions definition moved above this function for GCC/Clang compatibility
    // Original:
    // DefragmentationResult Defragment(DefragmentationOptions options = DefragmentationOptions{})
    // GCC fix: Provide overloaded methods to avoid default member initializer issues
    DefragmentationResult Defragment() { return Defragment(DefragmentationOptions()); }
    
    DefragmentationResult Defragment(DefragmentationOptions options)
        {
            DefragmentationResult result;
            
            // Calculate initial fragmentation
            result.fragmentationBefore = GetFragmentationLevel();
            
            size_t totalEntitiesMoved = 0;
            auto archetypes = m_archetypeManager->GetAllArchetypes();
            
            // Step 1: Defragment chunks within archetypes
            if (options.defragmentChunks)
            {
                size_t chunksProcessed = 0;
                
                for (auto* arch : archetypes)
                {
                    // Check limits
                    if (options.incremental)
                    {
                        if (chunksProcessed >= options.maxChunksToProcess) break;
                        if (totalEntitiesMoved >= options.maxEntitiesToMove) break;
                    }
                    
                    // Skip if archetype has few chunks or is already well-packed
                    if (arch->GetChunks().size() <= 1) continue;
                    
                    // Only process archetypes with significant fragmentation
                    float archFragmentation = arch->GetFragmentationLevel();
                    if (archFragmentation < (1.0f - options.chunkUtilizationThreshold)) continue;
                    
                    // Perform chunk coalescing
                    auto [chunksFreed, movedEntities] = arch->CoalesceChunks();
                    
                    // Update entity locations in ArchetypeManager
                    for (const auto& [entity, newLocation] : movedEntities)
                    {
                        m_archetypeManager->SetEntityLocation(entity, arch, newLocation);
                    }
                    
                    result.chunksRemoved += chunksFreed;
                    totalEntitiesMoved += movedEntities.size();
                    chunksProcessed += arch->GetChunks().size();
                    result.archetypesProcessed++;
                    
                    // Break if we've hit our entity move budget
                    if (options.incremental && totalEntitiesMoved >= options.maxEntitiesToMove)
                    {
                        break;
                    }
                }
                
                result.entitiesMoved = totalEntitiesMoved;
            }
            
            // Step 2: Remove empty archetypes
            // Only do this if we haven't exhausted our limits
            if (!options.incremental || totalEntitiesMoved < options.maxEntitiesToMove)
            {
                // Update metrics before cleanup
                m_archetypeManager->UpdateArchetypeMetrics();
                
                // Build cleanup options from our defragmentation options
                ArchetypeManager::CleanupOptions cleanupOpts;
                cleanupOpts.minEmptyDuration = options.minEmptyDuration;
                cleanupOpts.minArchetypesToKeep = options.minArchetypesToKeep;
                cleanupOpts.maxArchetypesToRemove = options.maxArchetypesToRemove;
                
                // If we're in incremental mode and close to entity limit, reduce archetype removals
                if (options.incremental && totalEntitiesMoved > options.maxEntitiesToMove * 0.8f)
                {
                    cleanupOpts.maxArchetypesToRemove = std::min(size_t(2), options.maxArchetypesToRemove);
                }
                
                // Use the existing implementation which handles all the complex removal logic
                result.archetypesRemoved = m_archetypeManager->CleanupEmptyArchetypes(cleanupOpts);
            }
            
            // Calculate final fragmentation
            result.fragmentationAfter = GetFragmentationLevel();
            
            return result;
        }
        
        /**
         * Get statistics about all archetypes
         * Useful for debugging and monitoring fragmentation
         */
        ASTRA_NODISCARD std::vector<ArchetypeInfo> GetArchetypeStats() const
        {
            return m_archetypeManager->GetArchetypeStats();
        }
        
        /**
         * Get current number of archetypes
         */
        ASTRA_NODISCARD size_t GetArchetypeCount() const
        {
            return m_archetypeManager->GetArchetypeCount();
        }
        
        /**
         * Get approximate memory usage by all archetypes
         */
        ASTRA_NODISCARD size_t GetArchetypeMemoryUsage() const
        {
            return m_archetypeManager->GetArchetypeMemoryUsage();
        }
        
        /**
         * Find an existing archetype with the exact set of components
         * Returns nullptr if no such archetype exists
         * 
         * @tparam Components Component types to search for
         * @return Pointer to archetype if found, nullptr otherwise
         * 
         * Example:
         *   if (auto* archetype = registry.FindArchetype<Position, Velocity>()) {
         *       // Direct archetype access for advanced operations
         *       archetype->ForEach([](Entity e, Position& p, Velocity& v) { ... });
         *   }
         */
        template<Component... Components>
        ASTRA_NODISCARD Archetype* FindArchetype() const
        {
            return m_archetypeManager->FindArchetype<Components...>();
        }
        
        /**
         * Find an existing archetype with the given component mask
         * Returns nullptr if no such archetype exists
         * 
         * @param mask Component mask to search for
         * @return Pointer to archetype if found, nullptr otherwise
         */
        ASTRA_NODISCARD Archetype* FindArchetype(const ComponentMask& mask) const
        {
            return m_archetypeManager->FindArchetype(mask);
        }
        
        /**
         * Get all archetypes for custom iteration
         * Returns a range of Archetype pointers
         * 
         * Example:
         *   for (Archetype* archetype : registry.GetAllArchetypes()) {
         *       // Custom archetype processing
         *   }
         */
        ASTRA_NODISCARD auto GetAllArchetypes()
        {
            return m_archetypeManager->GetAllArchetypes();
        }
        
        // ====================== Relationship API ======================

        template<typename... QueryArgs>
        ASTRA_NODISCARD Relations<QueryArgs...> GetRelations(Entity entity) const
        {
            return Relations<QueryArgs...>(m_archetypeManager, m_entityManager, entity, &m_relationshipGraph);
        }
        
        /**
         * Set the parent of an entity
         * 
         * @param child The child entity
         * @param parent The parent entity
         */
        void SetParent(Entity child, Entity parent)
        {
            if (m_entityManager->IsValid(child) && m_entityManager->IsValid(parent))
            {
                m_relationshipGraph.SetParent(child, parent);
                
                // Emit parent changed signal
                m_signalManager.Emit<Events::ParentChanged>(child, parent);
            }
        }
        
        /**
         * Remove the parent of an entity
         * 
         * @param child The child entity
         */
        void RemoveParent(Entity child)
        {
            if (m_entityManager->IsValid(child))
            {
                // Get current parent before removal for signal
                Entity parent = m_relationshipGraph.GetParent(child);
                m_relationshipGraph.RemoveParent(child);
                
                // Emit parent changed signal (parent is now invalid)
                if (parent.IsValid())
                {
                    m_signalManager.Emit<Events::ParentChanged>(child, Entity::Invalid());
                }
            }
        }
        
        /**
         * Add a bidirectional link between two entities
         * 
         * @param a First entity
         * @param b Second entity
         */
        void AddLink(Entity a, Entity b)
        {
            if (m_entityManager->IsValid(a) && m_entityManager->IsValid(b))
            {
                m_relationshipGraph.AddLink(a, b);
                
                // Emit link added signal
                m_signalManager.Emit<Events::LinkAdded>(a, b);
            }
        }
        
        /**
         * Remove a link between two entities
         * 
         * @param a First entity
         * @param b Second entity
         */
        void RemoveLink(Entity a, Entity b)
        {
            if (m_entityManager->IsValid(a) && m_entityManager->IsValid(b))
            {
                m_relationshipGraph.RemoveLink(a, b);
                
                // Emit link removed signal
                m_signalManager.Emit<Events::LinkRemoved>(a, b);
            }
        }
        
        /**
         * Get direct access to the relationship graph (internal use)
         * 
         * @return Reference to the relationship graph
         */
        ASTRA_NODISCARD RelationshipGraph& GetRelationshipGraph() { return m_relationshipGraph; }
        ASTRA_NODISCARD const RelationshipGraph& GetRelationshipGraph() const { return m_relationshipGraph; }
        
        // ====================== Signal API ======================
        
        /**
         * Enable signals for specific events
         * 
         * @param signals Bitwise OR of Signal flags to enable
         * 
         * Example:
         *   registry.EnableSignals(Signal::EntityCreated | Signal::ComponentAdded);
         */
        void EnableSignals(Signal signals)
        {
            m_signalManager.EnableSignals(signals);
        }
        
        /**
         * Disable signals for specific events
         * 
         * @param signals Bitwise OR of Signal flags to disable
         */
        void DisableSignals(Signal signals)
        {
            m_signalManager.DisableSignals(signals);
        }
        
        /**
         * Set exact signal configuration
         * 
         * @param signals Exact set of signals to enable (all others will be disabled)
         */
        void SetEnabledSignals(Signal signals)
        {
            m_signalManager.SetEnabledSignals(signals);
        }
        
        /**
         * Get currently enabled signals
         * 
         * @return Bitwise OR of currently enabled Signal flags
         */
        ASTRA_NODISCARD Signal GetEnabledSignals() const
        {
            return m_signalManager.GetEnabledSignals();
        }
        
        /**
         * Get the signal manager for connecting handlers
         * 
         * Example:
         *   auto& signals = registry.GetSignalManager();
         *   signals.OnEntityCreated().Add([](const EntityEvent& e) { ... });
         * 
         * @return Reference to the signal manager
         */
        SignalManager& GetSignalManager()
        {
            return m_signalManager;
        }
        
        const SignalManager& GetSignalManager() const
        {
            return m_signalManager;
        }
        
        // ====================== Serialization API ======================
        
        /**
         * Configuration for saving registry
         */
        // HOTFIX: Move SaveConfig above its first use as a default parameter (GCC/Clang requirement)
        struct SaveConfig
        {
            // Original: CompressionMode compressionMode = CompressionMode::LZ4;
            CompressionMode compressionMode = CompressionMode::LZ4;
            // Original: Compression::CompressionLevel compressionLevel = Compression::CompressionLevel::Fast;
            Compression::CompressionLevel compressionLevel = Compression::CompressionLevel::Fast;
            // Original: size_t compressionThreshold = 1024; // Only compress blocks larger than this
            size_t compressionThreshold = 1024; // Only compress blocks larger than this
            
            // GCC fix: explicit default constructor for brace initialization
            SaveConfig() = default;
        };
        
        /**
         * Save registry state to file with compression
         * @param path File path to save to
         * @param config Save configuration (compression settings)
         * @return Success or error code
         */
    // Original: Result<void, SerializationError> Save(const std::filesystem::path& path, const SaveConfig& config = SaveConfig{}) const
    // HOTFIX: SaveConfig definition moved above this function for GCC/Clang compatibility
    // Original:
    // Result<void, SerializationError> Save(const std::filesystem::path& path, SaveConfig config = SaveConfig{}) const
    // Original: Result<void, SerializationError> Save(const std::filesystem::path& path, SaveConfig config = SaveConfig{}) const
    // GCC fix: Provide overloaded methods to avoid default member initializer issues  
    Result<void, SerializationError> Save(const std::filesystem::path& path) const { return Save(path, SaveConfig()); }
    
    Result<void, SerializationError> Save(const std::filesystem::path& path, SaveConfig config) const
        {
            BinaryWriter::Config writerConfig;
            writerConfig.compressionMode = config.compressionMode;
            writerConfig.compressionLevel = config.compressionLevel;
            writerConfig.compressionThreshold = config.compressionThreshold;
            
            BinaryWriter writer(path, writerConfig);
            if (writer.HasError())
            {
                return Result<void, SerializationError>::Err(SerializationError::IOError);
            }
            
            // Write header
            BinaryHeader header;
            header.entityCount = static_cast<uint32_t>(m_entityManager->Size());
            header.archetypeCount = static_cast<uint32_t>(m_archetypeManager->GetArchetypeCount());
            
            writer.WriteHeader(header);
            
            // Serialize components
            m_entityManager->Serialize(writer);
            m_archetypeManager->Serialize(writer);
            m_relationshipGraph.Serialize(writer);
            
            // Finalize with checksum
            writer.FinalizeHeader();
            
            return writer.HasError() ? 
                Result<void, SerializationError>::Err(writer.GetError()) : 
                Result<void, SerializationError>::Ok();
        }
        
        /**
         * Save registry state to memory buffer with compression
         * @param config Save configuration (compression settings)
         * @return Buffer containing serialized data or error
         */
    // GCC fix: Provide overloaded methods to avoid default member initializer issues
    Result<std::vector<std::byte>, SerializationError> Save() const { return Save(SaveConfig()); }
    
    Result<std::vector<std::byte>, SerializationError> Save(SaveConfig config) const
        {
            std::vector<std::byte> buffer;
            
            BinaryWriter::Config writerConfig;
            writerConfig.compressionMode = config.compressionMode;
            writerConfig.compressionLevel = config.compressionLevel;
            writerConfig.compressionThreshold = config.compressionThreshold;
            
            BinaryWriter writer(buffer, writerConfig);
            
            // Write header
            BinaryHeader header;
            header.entityCount = static_cast<uint32_t>(m_entityManager->Size());
            header.archetypeCount = static_cast<uint32_t>(m_archetypeManager->GetArchetypeCount());
            
            writer.WriteHeader(header);
            
            // Serialize components
            m_entityManager->Serialize(writer);
            m_archetypeManager->Serialize(writer);
            m_relationshipGraph.Serialize(writer);
            
            // Finalize with checksum
            writer.FinalizeHeader();
            
            return writer.HasError() ? 
                Result<std::vector<std::byte>, SerializationError>::Err(writer.GetError()) : 
                Result<std::vector<std::byte>, SerializationError>::Ok(std::move(buffer));
        }
        
        /**
         * Load registry state from file
         * @param path File path to load from
         * @param componentRegistry Pre-configured component registry with all components registered
         * @return New registry instance or error
         */
        static Result<std::unique_ptr<Registry>, SerializationError> Load(const std::filesystem::path& path, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            BinaryReader reader(path);
            if (reader.HasError())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::IOError);
            }
            
            return LoadInternal(reader, std::move(componentRegistry));
        }
        
        /**
         * Load registry state from memory buffer
         * @param data Buffer containing serialized data
         * @param componentRegistry Pre-configured component registry with all components registered
         * @return New registry instance or error
         */
        static Result<std::unique_ptr<Registry>, SerializationError> Load(std::span<const std::byte> data, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            BinaryReader reader(data);
            return LoadInternal(reader, std::move(componentRegistry));
        }
        
        /**
         * Calculate overall fragmentation level across all archetypes
         * @return Fragmentation ratio (0.0 = no fragmentation, 1.0 = maximum fragmentation)
         */
        ASTRA_NODISCARD float GetFragmentationLevel() const
        {
            auto archetypes = m_archetypeManager->GetAllArchetypes();
            if (archetypes.empty()) return 0.0f;
            
            size_t totalEntities = 0;
            size_t totalChunks = 0;
            size_t optimalChunks = 0;
            
            for (const auto* arch : archetypes)
            {
                size_t entityCount = arch->GetEntityCount();
                size_t chunkCount = arch->GetChunks().size();
                
                totalEntities += entityCount;
                totalChunks += chunkCount;
                
                // Calculate optimal chunks for this archetype
                if (entityCount > 0)
                {
                    size_t entitiesPerChunk = arch->GetEntitiesPerChunk();
                    optimalChunks += (entityCount + entitiesPerChunk - 1) / entitiesPerChunk;
                }
            }
            
            if (totalChunks == 0) return 0.0f;
            
            // Fragmentation = excess chunks / total chunks
            size_t excessChunks = totalChunks > optimalChunks ? totalChunks - optimalChunks : 0;
            return static_cast<float>(excessChunks) / static_cast<float>(totalChunks);
        }
        
    private:
        /**
         * Internal helper to load registry from reader
         */
        static Result<std::unique_ptr<Registry>, SerializationError> LoadInternal(BinaryReader& reader, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            // Read and validate header
            auto headerResult = reader.ReadHeader();
            if (headerResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*headerResult.GetError());
            }
            
            // Deserialize EntityManager
            auto managerResult = EntityManager::Deserialize(reader);
            if (managerResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*managerResult.GetError());
            }
            
            // Create new registry instance with the provided component registry
            auto registry = std::make_unique<Registry>(componentRegistry);
            
            // Set the entity manager (convert unique_ptr to shared_ptr)
            registry->m_entityManager = std::move(*managerResult.GetValue());
            
            // Create new ArchetypeManager with the component registry and deserialize into it
            registry->m_archetypeManager = std::make_shared<ArchetypeManager>(componentRegistry, ArchetypeChunkPool::Config{});
            if (!registry->m_archetypeManager->Deserialize(reader))
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::CorruptedData);
            }
            
            // Deserialize RelationshipGraph
            auto graphResult = RelationshipGraph::Deserialize(reader);
            if (graphResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*graphResult.GetError());
            }
            registry->m_relationshipGraph = std::move(*graphResult.GetValue());
            
            // Verify checksum
            auto checksumResult = reader.VerifyChecksum();
            if (checksumResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*checksumResult.GetError());
            }
            
            return Result<std::unique_ptr<Registry>, SerializationError>::Ok(std::move(registry));
        }
        
        std::shared_ptr<EntityManager> m_entityManager;
        std::shared_ptr<ArchetypeManager> m_archetypeManager;
        RelationshipGraph m_relationshipGraph;
        SignalManager m_signalManager;
    };
}
