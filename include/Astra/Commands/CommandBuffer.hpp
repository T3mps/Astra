#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "../Core/Delegate.hpp"
#include "../Core/Result.hpp"
#include "../Registry/Registry.hpp"
#include "Command.hpp"

namespace Astra
{
    
    class CommandBuffer
    {
    public:
        enum class ExecutionError
        {
            None,
            InvalidRegistry,
            InvalidEntityManager,
            CommandFailed
        };
        
        explicit CommandBuffer(Registry* registry) :
            m_registry(registry)
        {
            ASTRA_ASSERT(registry != nullptr, "Registry cannot be null");
        }

        Entity CreateEntity()
        {
            if (m_registry)
            {
                auto& manager = m_registry->GetEntityManager();
                Entity entity = manager.Create();
                    if (entity == Entity::Invalid())
                    {
                        return Entity::Invalid();
                    }
                    
                    m_allocatedEntities.push_back(entity);
                    
                    m_commands.emplace_back(
                        [entity](Registry* registry) -> bool
                        {
#ifdef ASTRA_BUILD_DEBUG
                            printf("CommandBuffer: Executing CreateEntity for entity %u\n", entity.GetID());
#endif
                            // Entity already created, just add to archetype system
                            registry->GetArchetypeManager()->AddEntity(entity);
                            registry->GetSignalManager()->Emit<Events::EntityCreated>(entity);
                            return true;
                        },
                        Command::Type::Entity,
                        "CreateEntity"
                    );
                    
                return entity;
            }
            return Entity::Invalid();
        }

        void DestroyEntity(Entity entity)
        {
            m_commands.emplace_back(
                [entity](Registry* registry) -> bool
                {
                    if (entity == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->DestroyEntity(entity);
                    return true;
                },
                Command::Type::Entity,
                "DestroyEntity"
            );
        }

        void CreateEntities(size_t count, Entity* outEntities)
        {
            if (m_registry)
            {
                auto& manager = m_registry->GetEntityManager();
                // Allocate entities immediately
                manager.CreateBatch(count, outEntities);
                
                    // Track for potential rollback
                    for (size_t i = 0; i < count; ++i)
                    {
                        m_allocatedEntities.push_back(outEntities[i]);
                    }
                
                    // Copy entities for command since outEntities might not be valid at execution
                    std::vector<Entity> entities(outEntities, outEntities + count);
                    m_commands.emplace_back(
                        [entities](Registry* registry) -> bool
                        {
                            // Entities already created, just add them to archetype system
                            auto* archetypeManager = registry->GetArchetypeManager();
                            auto* signalManager = registry->GetSignalManager();
                        
                            for (Entity e : entities)
                            {
                                archetypeManager->AddEntity(e);
                                signalManager->Emit<Events::EntityCreated>(e);
                            }
                            return true;
                        },
                        Command::Type::Entity,
                        "CreateEntities"
                    );
            }
        }

        void DestroyEntities(std::span<const Entity> entities)
        {
            // Copy entities since the span might not be valid at execution time
            std::vector<Entity> entityCopy(entities.begin(), entities.end());
            m_commands.emplace_back(
                [entityCopy = std::move(entityCopy)](Registry* registry) -> bool
                {
                    SmallVector<Entity, 256> validEntities;
                    validEntities.reserve(entityCopy.size());
                    
                    for (Entity e : entityCopy)
                    {
                        if (e != Entity::Invalid())
                        {
                            validEntities.push_back(e);
                        }
                    }
                    
                    if (!validEntities.empty())
                    {
                        registry->DestroyEntities(validEntities);
                    }
                    return true;
                },
                Command::Type::Entity,
                "DestroyEntities"
            );
        }

        template<Component T>
        void AddComponent(Entity entity, T&& component)
        {
            m_commands.emplace_back(
                [entity, comp = std::forward<T>(component)](Registry* registry) -> bool
                {
                    if (entity == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->AddComponent(entity, std::move(comp));
                    return true;
                },
                Command::Type::Component,
                "AddComponent"
            );
        }
        
        template<Component T, typename... Args>
        void EmplaceComponent(Entity entity, Args&&... args)
        {
            m_commands.emplace_back(
                [entity, comp = T(std::forward<Args>(args)...)](Registry* registry) -> bool
                {
                    if (entity == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->AddComponent<T>(entity, comp);
                    return true;
                },
                Command::Type::Component,
                "EmplaceComponent"
            );
        }

        template<Component T>
        void RemoveComponent(Entity entity)
        {
            m_commands.emplace_back(
                [entity](Registry* registry) -> bool
                {
                    if (entity == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->RemoveComponent<T>(entity);
                    return true;
                },
                Command::Type::Component,
                "RemoveComponent"
            );
        }

        template<Component T>
        void AddComponents(std::span<const Entity> entities, const T& component)
        {
            std::vector<Entity> entityCopy(entities.begin(), entities.end());
            m_commands.emplace_back(
                [entityCopy = std::move(entityCopy), component](Registry* registry) -> bool
                {
#ifdef ASTRA_BUILD_DEBUG
                    printf("CommandBuffer: Executing AddComponents for %zu entities\n", entityCopy.size());
#endif
                    SmallVector<Entity, 256> validEntities;
                    validEntities.reserve(entityCopy.size());
                    for (Entity e : entityCopy)
                    {
                        if (e != Entity::Invalid())
                        {
                            validEntities.push_back(e);
#ifdef ASTRA_BUILD_DEBUG
                            printf("  Entity %u is valid\n", e.GetID());
#endif
                        }
                    }
                    if (!validEntities.empty())
                    {
#ifdef ASTRA_BUILD_DEBUG
                        printf("  Calling registry->AddComponents with %zu valid entities\n", validEntities.size());
#endif
                        registry->AddComponents<T>(validEntities, component);
                    }
                    return true;
                },
                Command::Type::Component,
                "AddComponents"
            );
        }
        
        template<Component T, typename... Args>
        void EmplaceComponents(std::span<const Entity> entities, Args&&... args)
        {
            std::vector<Entity> entityCopy(entities.begin(), entities.end());
            m_commands.emplace_back(
                [entityCopy = std::move(entityCopy), comp = T(std::forward<Args>(args)...)](Registry* registry) -> bool
                {
                    SmallVector<Entity, 256> validEntities;
                    validEntities.reserve(entityCopy.size());
                    for (Entity e : entityCopy)
                    {
                        if (e != Entity::Invalid())
                        {
                            validEntities.push_back(e);
                        }
                    }
                    if (!validEntities.empty())
                    {
                        registry->AddComponents<T>(validEntities, comp);
                    }
                    return true;
                },
                Command::Type::Component,
                "EmplaceComponents"
            );
        }

        template<Component T>
        void RemoveComponents(std::span<const Entity> entities)
        {
            std::vector<Entity> entityCopy(entities.begin(), entities.end());
            m_commands.emplace_back(
                [entityCopy = std::move(entityCopy)](Registry* registry) -> bool
                {
                    SmallVector<Entity, 256> validEntities;
                    validEntities.reserve(entityCopy.size());
                    for (Entity e : entityCopy)
                    {
                        if (e != Entity::Invalid())
                        {
                            validEntities.push_back(e);
                        }
                    }
                    if (!validEntities.empty())
                    {
                        registry->RemoveComponents<T>(validEntities);
                    }
                    return true;
                },
                Command::Type::Component,
                "RemoveComponents"
            );
        }

        // ============= Relationship Commands =============

        void SetParent(Entity child, Entity parent)
        {
            m_commands.emplace_back(
                [child, parent](Registry* registry) -> bool
                {
                    if (child == Entity::Invalid() || parent == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->SetParent(child, parent);
                    return true;
                },
                Command::Type::Relationship,
                "SetParent"
            );
        }
        
        void AddChild(Entity parent, Entity child)
        {
            // AddChild is just SetParent with reversed parameters
            SetParent(child, parent);
        }

        void RemoveParent(Entity child)
        {
            m_commands.emplace_back(
                [child](Registry* registry) -> bool
                {
                    if (child == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->RemoveParent(child);
                    return true;
                },
                Command::Type::Relationship,
                "RemoveParent"
            );
        }
        
        void RemoveChild(Entity parent, Entity child)
        {
            m_commands.emplace_back(
                [parent, child](Registry* registry) -> bool
                {
                    if (parent == Entity::Invalid() || child == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->RemoveChild(parent, child);
                    return true;
                },
                Command::Type::Relationship,
                "RemoveChild"
            );
        }
        
        void RemoveAllChildren(Entity parent)
        {
            m_commands.emplace_back(
                [parent](Registry* registry) -> bool
                {
                    if (parent == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->RemoveAllChildren(parent);
                    return true;
                },
                Command::Type::Relationship,
                "RemoveAllChildren"
            );
        }

        void AddLink(Entity a, Entity b)
        {
            m_commands.emplace_back(
                [a, b](Registry* registry) -> bool
                {
                    if (a == Entity::Invalid() || b == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->AddLink(a, b);
                    return true;
                },
                Command::Type::Relationship,
                "AddLink"
            );
        }

        void RemoveLink(Entity a, Entity b)
        {
            m_commands.emplace_back(
                [a, b](Registry* registry) -> bool
                {
                    if (a == Entity::Invalid() || b == Entity::Invalid())
                    {
                        return false;
                    }
                    registry->RemoveLink(a, b);
                    return true;
                },
                Command::Type::Relationship,
                "RemoveLink"
            );
        }
        
        // ============= Resource Commands =============
        
        template<Component T>
        void SetResource(T&& resource)
        {
            m_commands.emplace_back(
                [res = std::forward<T>(resource)](Registry* registry) mutable -> bool
                {
                    registry->SetResource<T>(std::move(res));
                    return true;
                },
                Command::Type::Resource,
                "SetResource"
            );
        }
        
        template<Component T, typename... Args>
        void EmplaceResource(Args&&... args)
        {
            m_commands.emplace_back(
                [res = T(std::forward<Args>(args)...)](Registry* registry) mutable -> bool
                {
                    registry->SetResource<T>(std::move(res));
                    return true;
                },
                Command::Type::Resource,
                "EmplaceResource"
            );
        }
        
        template<Component T>
        void RemoveResource()
        {
            m_commands.emplace_back(
                [](Registry* registry) -> bool
                {
                    registry->RemoveResource<T>();
                    return true;
                },
                Command::Type::Resource,
                "RemoveResource"
            );
        }
        
        void ClearResources()
        {
            m_commands.emplace_back(
                [](Registry* registry) -> bool
                {
                    registry->ClearResources();
                    return true;
                },
                Command::Type::Resource,
                "ClearResources"
            );
        }

        // ============= Execution and Management =============

        Result<void, ExecutionError> Execute(bool clearAfterExecution = true)
        {
            if (!m_registry)
            {
                RollbackAllocatedEntities();
                return Result<void, ExecutionError>::Err(ExecutionError::InvalidRegistry);
            }
            
            // EntityManager is always valid if Registry exists
            
            // Execute all commands
            for (size_t i = 0; i < m_commands.size(); ++i)
            {
                if (!m_commands[i].execute(m_registry))
                {
                    // Command failed - rollback
                    RollbackAllocatedEntities();
                    
#ifdef ASTRA_BUILD_DEBUG
                    if (m_commands[i].debugName)
                    {
                        printf("Command failed: %s (index %zu)\n", m_commands[i].debugName, i);
                    }
#endif
                    
                    return Result<void, ExecutionError>::Err(ExecutionError::CommandFailed);
                }
            }
            
            // Success
            m_allocatedEntities.clear(); 
            
            if (clearAfterExecution)
            {
                Clear();
            }
            
            return Result<void, ExecutionError>::Ok();
        }

        void Clear()
        {
            m_commands.clear();
            m_allocatedEntities.clear();
        }
        
        void Reserve(size_t commandCount)
        {
            m_commands.reserve(commandCount);
        }
        
        /**
         * Merge commands from another buffer into this one
         * The other buffer is moved from and left empty
         */
        void MergeFrom(CommandBuffer&& other)
        {
            // Move all commands
            m_commands.insert(
                m_commands.end(),
                std::make_move_iterator(other.m_commands.begin()),
                std::make_move_iterator(other.m_commands.end())
            );
            
            // Merge allocated entities
            m_allocatedEntities.insert(
                m_allocatedEntities.end(),
                other.m_allocatedEntities.begin(),
                other.m_allocatedEntities.end()
            );
            
            // Clear the other buffer
            other.Clear();
        }
        
        [[nodiscard]] size_t GetCommandCount() const noexcept
        {
            return m_commands.size();
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return m_commands.empty();
        }
        
        [[nodiscard]] size_t GetMemoryUsage() const noexcept
        {
            return m_commands.capacity() * sizeof(Command) + m_allocatedEntities.capacity() * sizeof(Entity);
        }
        
        void RollbackAllocatedEntities()
        {
            if (m_registry)
            {
                auto& manager = m_registry->GetEntityManager();
                for (Entity e : m_allocatedEntities)
                {
                    manager.Destroy(e);
                }
            }
            m_allocatedEntities.clear();
        }

    private:        
        Registry* m_registry;
        std::vector<Command> m_commands;
        std::vector<Entity> m_allocatedEntities;
    };
    
    class ParallelCommandBuffer
    {
    public:
        explicit ParallelCommandBuffer(Registry* registry) : 
            m_registry(registry)
        {
            ASTRA_ASSERT(registry != nullptr, "Registry cannot be null");
            // Pre-reserve space for typical thread counts
            const size_t expectedThreads = std::thread::hardware_concurrency();
            m_buffers.reserve(expectedThreads);
        }
        
        CommandBuffer& GetThreadBuffer() const
        {
            // Fast path: check thread-local cache
            if (t_cache.context == this && t_cache.buffer != nullptr)
            {
                return *t_cache.buffer;
            }
            
            // Slow path: create new buffer for this thread
            return InitializeThreadBuffer();
        }
        
        Result<void, CommandBuffer::ExecutionError> Execute()
        {
            for (size_t i = 0; i < m_buffers.size(); ++i)
            {
                if (m_buffers[i] && !m_buffers[i]->IsEmpty())
                {
                    auto result = m_buffers[i]->Execute();
                    if (result.IsErr())
                    {
                        // Rollback remaining buffers' allocated entities
                        for (size_t j = i + 1; j < m_buffers.size(); ++j)
                        {
                            if (m_buffers[j])
                            {
                                m_buffers[j]->RollbackAllocatedEntities();
                            }
                        }
                        return result;
                    }
                }
            }
            return Result<void, CommandBuffer::ExecutionError>::Ok();
        }

        void MergeInto(CommandBuffer& target)
        {
            // Properly merge all thread buffers into the target
            for (auto& buffer : m_buffers)
            {
                if (buffer && !buffer->IsEmpty())
                {
                    target.MergeFrom(std::move(*buffer));
                }
            }
        }
        
        void Clear()
        {
            for (auto& buffer : m_buffers)
            {
                if (buffer)
                {
                    buffer->Clear();
                }
            }
        }

        ASTRA_NODISCARD size_t GetCommandCount() const
        {
            size_t total = 0;
            for (const auto& buffer : m_buffers)
            {
                if (buffer)
                {
                    total += buffer->GetCommandCount();
                }
            }
            return total;
        }

        ASTRA_NODISCARD bool IsEmpty() const
        {
            for (const auto& buffer : m_buffers)
            {
                if (buffer && !buffer->IsEmpty())
                {
                    return false;
                }
            }
            return true;
        }

        ASTRA_NODISCARD size_t GetThreadCount() const
        {
            return m_buffers.size();
        }
        
    private:
        CommandBuffer& InitializeThreadBuffer() const
        {
            // Allocate a new index for this thread
            const size_t index = m_nextIndex.fetch_add(1, std::memory_order_relaxed);
            
            // Lock only for vector modification
            std::unique_lock lock(m_mutex);
            
            // Ensure vector is large enough
            if (index >= m_buffers.size())
            {
                m_buffers.resize(index + 1);
            }
            
            // Create the buffer if it doesn't exist
            if (!m_buffers[index])
            {
                m_buffers[index] = std::make_unique<CommandBuffer>(m_registry);
            }
            
            CommandBuffer* buffer = m_buffers[index].get();
            
            // Unlock before updating thread-local cache
            lock.unlock();
            
            // Update thread-local cache
            t_cache.context = const_cast<ParallelCommandBuffer*>(this);
            t_cache.buffer = buffer;
            t_cache.index = index;
            
            return *buffer;
        }

        Registry* m_registry;
        mutable std::mutex m_mutex;
        mutable std::vector<std::unique_ptr<CommandBuffer>> m_buffers;
        mutable std::atomic<size_t> m_nextIndex{0};

        // Thread-local cache to avoid repeated lookups
        struct ThreadCache
        {
            ParallelCommandBuffer* context = nullptr;
            CommandBuffer* buffer = nullptr;
            size_t index = std::numeric_limits<size_t>::max();
        };

        static thread_local ThreadCache t_cache;
    };
    
    // Thread-local storage definition
    inline thread_local ParallelCommandBuffer::ThreadCache ParallelCommandBuffer::t_cache;
    
} // namespace Astra