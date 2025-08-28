#pragma once

#include "../Component/Component.hpp"
#include "../Container/FlatMap.hpp"
#include "../Core/Base.hpp"

namespace Astra
{
    class Archetype;
    
    class ArchetypeGraph
    {
    public:
        ArchetypeGraph() = default;
        ~ArchetypeGraph() = default;
        
        // Disable copy, allow move
        ArchetypeGraph(const ArchetypeGraph&) = delete;
        ArchetypeGraph& operator=(const ArchetypeGraph&) = delete;
        ArchetypeGraph(ArchetypeGraph&&) = default;
        ArchetypeGraph& operator=(ArchetypeGraph&&) = default;
        
        void SetAddEdge(Archetype* from, ComponentID componentId, Archetype* to)
        {
            if (!from || !to) ASTRA_UNLIKELY
                return;
            
            auto& edges = m_addEdges[from];
            edges.Insert(std::make_pair(componentId, to));
        }
        
        void SetRemoveEdge(Archetype* from, ComponentID componentId, Archetype* to)
        {
            if (!from || !to) ASTRA_UNLIKELY
                return;
            
            auto& edges = m_removeEdges[from];
            edges.Insert(std::make_pair(componentId, to));
        }
        
        template<typename EdgeMap>
        ASTRA_NODISCARD Archetype* GetEdgeInternal(const EdgeMap& edges, Archetype* from, ComponentID componentId) const noexcept
        {
            if (!from) ASTRA_UNLIKELY
                return nullptr;
            
            auto it = edges.Find(from);
            if (it == edges.end())
                return nullptr;
                
            auto edgeIt = it->second.Find(componentId);
            return edgeIt != it->second.end() ? edgeIt->second : nullptr;
        }
        
        ASTRA_NODISCARD Archetype* GetAddEdge(Archetype* from, ComponentID componentId) const noexcept
        {
            return GetEdgeInternal(m_addEdges, from, componentId);
        }

        ASTRA_NODISCARD Archetype* GetRemoveEdge(Archetype* from, ComponentID componentId) const noexcept
        {
            return GetEdgeInternal(m_removeEdges, from, componentId);
        }
        
        template<typename EdgeMap>
        size_t RemoveEdgesToInternal(EdgeMap& edges, Archetype* target)
        {
            size_t removed = 0;
            for (auto& [from, edgeMap] : edges)
            {
                auto it = edgeMap.begin();
                while (it != edgeMap.end())
                {
                    if (it->second == target)
                    {
                        it = edgeMap.Erase(it);
                        ++removed;
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            return removed;
        }
        
        size_t RemoveEdgesTo(Archetype* target)
        {
            if (!target) ASTRA_UNLIKELY
                return 0;
            
            return RemoveEdgesToInternal(m_addEdges, target) + 
                   RemoveEdgesToInternal(m_removeEdges, target);
        }
        
        void RemoveEdgesFrom(Archetype* from)
        {
            if (!from) ASTRA_UNLIKELY
                return;
            
            m_addEdges.Erase(from);
            m_removeEdges.Erase(from);
        }

        void Clear()
        {
            m_addEdges.Clear();
            m_removeEdges.Clear();
        }

        ASTRA_NODISCARD size_t GetEdgeCount() const noexcept
        {
            size_t count = 0;
            for (const auto& [from, edges] : m_addEdges)
            {
                count += edges.Size();
            }
            for (const auto& [from, edges] : m_removeEdges)
            {
                count += edges.Size();
            }
            return count;
        }
        
    private:
        FlatMap<Archetype*, FlatMap<ComponentID, Archetype*>> m_addEdges;
        FlatMap<Archetype*, FlatMap<ComponentID, Archetype*>> m_removeEdges;
    };
}