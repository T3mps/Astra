#pragma once

#include <cstdlib>
#include <vector>

#include <Astra/Astra.hpp>

#include "Components.hpp"

namespace Studio
{
    enum class Preset : int { Movers = 0, Fighters = 1, Decor = 2, Mixed = 3 };
    inline const char* PresetNames[] = { "Movers (Pos+Vel)", "Fighters (Pos+Vel+Health)",
                                         "Decor (Pos+Sprite+Frozen)", "Mixed (round-robin)" };

    class WorkloadRunner
    {
    public:
        explicit WorkloadRunner(Astra::Registry& registry) : m_registry(registry) {}

        void Spawn(Preset preset, int count)
        {
            auto frand = [] { return float(std::rand()) / float(RAND_MAX) * 100.0f; };
            for (int i = 0; i < count; ++i)
            {
                Preset p = (preset == Preset::Mixed) ? Preset(i % 3) : preset;
                Astra::Entity e = Astra::Entity::Invalid();
                switch (p)
                {
                case Preset::Movers:
                    e = m_registry.CreateEntityWith(
                        Position{frand(), frand(), 0}, Velocity{1, 1, 0});
                    break;
                case Preset::Fighters:
                    e = m_registry.CreateEntityWith(
                        Position{frand(), frand(), 0}, Velocity{-1, 2, 0}, Health{});
                    break;
                default:
                    e = m_registry.CreateEntityWith(
                        Position{frand(), frand(), 0}, Sprite{}, Frozen{});
                    break;
                }
                if (e.IsValid()) m_spawned.push_back(e);
            }
        }

        void Clear()
        {
            for (Astra::Entity e : m_spawned)
                if (m_registry.IsValid(e)) (void)m_registry.DestroyEntity(e);
            m_spawned.clear();
        }

        void Step(float dt)
        {
            auto view = m_registry.CreateView<Position, const Velocity>();
            view.ForEach([dt](Position& p, const Velocity& v)
            {
                p.x += v.dx * dt; p.y += v.dy * dt; p.z += v.dz * dt;
            });
            ++m_steps;
        }

        size_t   Spawned() const { return m_spawned.size(); }
        uint64_t StepsRun() const { return m_steps; }

    private:
        Astra::Registry& m_registry;
        std::vector<Astra::Entity> m_spawned;
        uint64_t m_steps = 0;
    };
}
