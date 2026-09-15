#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "../Archetype/Archetype.hpp"
#include "../Archetype/ArchetypeManager.hpp"
#include "../Component/Component.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/WorkScheduler.hpp"
#include "../Entity/Entity.hpp"
#include "EnabledRuns.hpp"
#include "Query.hpp"
#include "ViewIterator.hpp"

#include <bit>
#include <cstdint>

namespace Astra
{
    enum class QueryError { NotMatched, Empty, MultipleMatched };

    template<typename... QueryArgs>
    class View
    {
        static_assert(ValidQuery<QueryArgs...>, "View template arguments must be valid components or query modifiers");

        // Query type extraction - must be declared early for Iterator type
        using RequiredTypes = typename Detail::QueryClassifier<QueryArgs...>::RequiredComponents;
        using OptionalTypes = typename Detail::QueryClassifier<QueryArgs...>::OptionalComponents;
        using QueryBuilder = Astra::QueryBuilder<QueryArgs...>;  // qualified to avoid -Wchanges-meaning

        // ================= Enableable-components query filtering (spec §5) =================
        //
        // EnabledRequiredFilter: bare required enableable components (IncludeDisabled
        // opts out) -- drives run extraction + chunk skipping. EnabledOptionalFilter:
        // enableable Optional<T> -- pointer nulled per entity while disabled.
        //
        // HasEnabledFilter is the load-bearing compile-time gate: when it is false the
        // whole filtered path is never instantiated and ForEach/ParallelForEach/Size
        // compile to the exact pre-existing loops (invariant 1, zero cost when unused).
        using EnabledRequiredFilter = Detail::EnableableRequiredFilter_t<QueryArgs...>;
        using EnabledOptionalFilter = Detail::FilterEnableable_t<OptionalTypes>;

        static constexpr bool HasRequiredFilter = std::tuple_size_v<EnabledRequiredFilter> > 0;
        static constexpr bool HasOptionalFilter = std::tuple_size_v<EnabledOptionalFilter> > 0;
        static constexpr bool HasEnabledFilter  = HasRequiredFilter || HasOptionalFilter;

        // ================= Change-detection filters (spec 2026-09-10 §3.4) =================
        //
        // Changed<T>/Added<T> are match-only terms (T required, never yielded, zero
        // ViewAccess footprint) that reject whole chunks whose T column version is not
        // newer than the querying tick. HasChangeFilter is the compile-time gate, exactly
        // like HasEnabledFilter: a view naming no Changed/Added instantiates none of the
        // reject code. Iteration-only this stage: ForEach(ctx, fn) / Since(tick).ForEach.
        using ChangedTypes = typename Detail::QueryClassifier<QueryArgs...>::ChangedComponents;
        using AddedTypes   = typename Detail::QueryClassifier<QueryArgs...>::AddedComponents;
        static_assert(Detail::ChangeTermsAreRequired<QueryArgs...>,
            "Changed<T>/Added<T>: T must also be listed as a required component of this view "
            "(e.g. CreateView<const T, Changed<T>, ...>) -- filter what you fetch");
    public:
        static constexpr bool HasChangeFilter = (std::tuple_size_v<ChangedTypes> + std::tuple_size_v<AddedTypes>) > 0;
    private:
        // Any per-chunk filter at all: routes iteration through VisitChunkFiltered.
        static constexpr bool HasChunkFilter = HasEnabledFilter || HasChangeFilter;

        // Any Changed<T>/Added<T> term over a change-TRACKED T: inside a chunk that
        // passed the version reject, that term is evaluated per entity from the tick
        // column (spec §3.4 step 3). False for every untracked change filter, which
        // stays chunk-granular and instantiates none of the per-entity mask code.
        template<typename Tuple> struct AnyTracked;
        template<typename... Ts> struct AnyTracked<std::tuple<Ts...>> : std::bool_constant<(IsChangeTrackedV<Ts> || ...)> {};
        static constexpr bool HasTrackedChangeTerm = AnyTracked<ChangedTypes>::value || AnyTracked<AddedTypes>::value;

        // Any yielded, non-const, change-tracked component: routes iteration through
        // the view-owned chunk loops (Archetype::ForEachStamped cannot yield Mut) and
        // switches the required yield to Mut<T>. False for every pre-existing view.
        template<typename Tuple> struct AnyTrackedYield;
        template<typename... Ts> struct AnyTrackedYield<std::tuple<Ts...>>
            : std::bool_constant<((Detail::IsMutableYield<Ts> && IsChangeTrackedV<Ts>) || ...)> {};
        static constexpr bool HasTrackedYield = AnyTrackedYield<RequiredTypes>::value || AnyTrackedYield<OptionalTypes>::value;

        // Parallel execution thresholds - based on empirical testing
        static constexpr size_t AVG_ENTITIES_PER_CHUNK = 256;                           // Typical for 16KB chunks with ~50 byte entities
        static constexpr size_t MIN_CHUNKS_PER_THREAD = 4;                              // Each thread should process at least 4 chunks (64KB)
        static constexpr size_t MIN_CHUNKS_FOR_PARALLEL = MIN_CHUNKS_PER_THREAD * 2;    // Need enough for at least 2 threads

        // Derive entity thresholds from chunk-based values
        static constexpr size_t MIN_ENTITIES_QUICK_CHECK = AVG_ENTITIES_PER_CHUNK / 2;  // Less than half a chunk = definitely sequential
        static constexpr size_t MIN_ENTITIES_FOR_PARALLEL = MIN_CHUNKS_FOR_PARALLEL * AVG_ENTITIES_PER_CHUNK / 2;  // ~4 chunks worth

    public:
        explicit View(std::shared_ptr<ArchetypeManager> manager,
                      std::shared_ptr<IWorkScheduler> scheduler = nullptr) :
            m_archetypeManager(manager),
            m_scheduler(std::move(scheduler)),   // null => sequential fallback
            m_lastRefreshCounter(0),
            m_lastGeneration(0)
        {
            if (manager)
            {
                CollectArchetypes();
                m_lastRefreshCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
                m_lastGeneration = m_archetypeManager->m_generation;
                m_lastRemovalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
            }
            // else: null manager -> empty/invalid view; all m_last* counters keep their
            // default-initialized 0 (View.hpp member initializers).
        }
        
        /**
         * Check if the View is still valid (Registry not destroyed)
         * @return true if the underlying ArchetypeManager is still alive
         */
        ASTRA_NODISCARD bool IsValid() const noexcept
        {
            return m_archetypeManager != nullptr;
        }

        /**
         * Invoke func(Entity, Components&...) -- or func(Components&...) (the
         * leading Entity is optional) -- for every entity matching this View.
         *
         * CONTRACT: structural mutation is NOT supported during this call. Do not
         * create or destroy entities, or add/remove components, from within func --
         * the Archetype/chunk pointers this loop walks are captured up front and are
         * not re-validated mid-iteration, so a direct structural change here is
         * undefined behavior. In-place edits to already-present component VALUES
         * (e.g. `pos.x += 1`) are fine.
         *
         * To change entity structure while iterating, record the changes into a
         * `CommandBuffer` and call `Execute()` AFTER this loop returns. Recording
         * into a CommandBuffer does not itself touch the ArchetypeManager, so a
         * properly deferred CommandBuffer never trips the Debug guard below.
         *
         * In Debug builds this is additionally enforced by an ASTRA_ASSERT that
         * compares the ArchetypeManager's structural-change counter before and
         * after the loop; the check (and the counter read) is compiled out
         * entirely in Release/Dist builds, so this contract carries zero Release
         * cost.
         *
         * Change detection (spec §3.4): a view with a Changed<T>/Added<T> filter
         * needs a "since" tick and refuses this overload at compile time -- call
         * ForEach(ctx, fn) from a SystemContext system, or Since(tick).ForEach(fn).
         */
        template<typename Func>
        ASTRA_FORCEINLINE void ForEach(Func&& func)
        {
            static_assert(!HasChangeFilter,
                "This view has a Changed<T>/Added<T> filter and needs a 'since' tick: call "
                "ForEach(ctx, fn) from a SystemContext system, or Since(tick).ForEach(fn).");
            ForEachSince(Tick{0}, std::forward<Func>(func));
        }

        /**
         * Tick-aware ForEach: `since` is taken from ctx.LastRun() (any TickContext --
         * SystemContext, or a test stand-in), so Changed<T>/Added<T> terms see exactly
         * what changed since this system last ran. Also valid on an unfiltered view.
         */
        template<TickContext Ctx, typename Func>
        ASTRA_FORCEINLINE void ForEach(const Ctx& ctx, Func&& func) { ForEachSince(ctx.LastRun(), std::forward<Func>(func)); }

        template<typename Func>
        ASTRA_FORCEINLINE void ParallelForEach(Func&& func)
        {
            static_assert(!HasChangeFilter,
                "This view has a Changed<T>/Added<T> filter and needs a 'since' tick: call "
                "ParallelForEach(ctx, fn) from a SystemContext system, or Since(tick).ParallelForEach(fn).");
            ParallelForEachSince(Tick{0}, std::forward<Func>(func));
        }

        template<TickContext Ctx, typename Func>
        ASTRA_FORCEINLINE void ParallelForEach(const Ctx& ctx, Func&& func) { ParallelForEachSince(ctx.LastRun(), std::forward<Func>(func)); }

        /**
         * Explicit-tick form for Registry&-only systems and tests:
         * `view.Since(tick).ForEach(fn)` iterates with Changed<T>/Added<T> evaluated
         * against `tick`. A thin non-owning handle over this view; use it immediately.
         */
        class SinceView
        {
        public:
            SinceView(View& v, Tick since) noexcept : m_view(&v), m_since(since) {}
            template<typename Func> void ForEach(Func&& func)         { m_view->ForEachSince(m_since, std::forward<Func>(func)); }
            template<typename Func> void ParallelForEach(Func&& func) { m_view->ParallelForEachSince(m_since, std::forward<Func>(func)); }
        private:
            View* m_view;
            Tick  m_since;
        };
        ASTRA_NODISCARD SinceView Since(Tick since) noexcept { return SinceView(*this, since); }

    private:
        // ForEach with an explicit "since" tick: the one stamped walk behind
        // ForEach(fn), ForEach(ctx, fn) and Since(tick).ForEach(fn).
        template<typename Func>
        ASTRA_FORCEINLINE void ForEachSince(Tick since, Func&& func) { ForEachBody<true>(since, std::forward<Func>(func)); }

        // ParallelForEach with an explicit "since" tick (see ForEachSince).
        template<typename Func>
        ASTRA_FORCEINLINE void ParallelForEachSince(Tick since, Func&& func)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            EnsureArchetypes();

            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return;

            auto adapted = MakeEntityOptionalAdapter(func);

            // Quick check: if we have very few matching entities, don't even try parallel
            size_t quickCount = 0;
            for (Archetype* archetype : m_archetypes)
            {
                quickCount += archetype->GetEntityCount();
            }

            if (quickCount < MIN_ENTITIES_QUICK_CHECK)
            {
                return ForEachSince(since, adapted);
            }

            // No scheduler injected: Astra spawns no threads — run sequentially inline.
            if (!m_scheduler)
                return ForEachSince(since, adapted);

            std::vector<std::pair<Archetype*, size_t>> chunkWork;
            // Better estimation based on typical entities per 16KB chunk
            size_t estimatedChunks = (quickCount / AVG_ENTITIES_PER_CHUNK) + m_archetypes.size();
            chunkWork.reserve(estimatedChunks);
            size_t totalMatchingEntities = 0;
            
            for (Archetype* archetype : m_archetypes)
            {
                size_t chunkCount = archetype->GetChunkCount();
                for (size_t i = 0; i < chunkCount; ++i)
                {
                    size_t chunkEntityCount = archetype->GetChunkEntityCount(i);
                    if (chunkEntityCount > 0)
                    {
                        chunkWork.emplace_back(archetype, i);
                        totalMatchingEntities += chunkEntityCount;
                    }
                }
            }
            
            // Fall back to sequential for tiny workloads
            if (chunkWork.empty() || totalMatchingEntities < MIN_ENTITIES_FOR_PARALLEL || chunkWork.size() < MIN_CHUNKS_FOR_PARALLEL)
            {
                return ForEachSince(since, adapted);
            }

            m_scheduler->ParallelFor(chunkWork.size(), MIN_CHUNKS_PER_THREAD,
                [&](size_t begin, size_t end, uint32_t /*worker*/)
                {
                    for (size_t w = begin; w < end; ++w)
                    {
                        auto [archetype, chunkIndex] = chunkWork[w];
                        ParallelForEachChunkImpl(archetype, chunkIndex, adapted, since, RequiredTypes{}, OptionalTypes{});
                    }
                });
        }

    public:
        /**
         * Like ParallelForEach, but threads a per-chunk sub-context to the body
         * (Theme B2 Phase B, Task 3 -- the machinery behind
         * SystemContext::ParallelForEach). For each unit of chunk work at FLAT
         * chunkWork index `w`, builds `auto sub = factory(w);` and invokes
         * `body(entity, components..., sub)` for every entity in that chunk. The
         * factory runs ON the worker executing the chunk, so a sub-context whose
         * recorder is a per-thread CommandBuffer records into THAT worker's own
         * buffer.
         *
         * DETERMINISM (critical): the factory argument is the FLAT chunkWork
         * index `w`, NOT the per-archetype chunk index (chunkWork[w].second). In
         * a multi-archetype view chunk 0 of archetype A and chunk 0 of archetype
         * B share the per-archetype index 0, so stamping that would give two
         * chunks the same {insertionOrder, 0, ...} key -- and since each
         * sub-context restarts its recordSequence at 0, their commands would
         * collide and the flush's stable-sort tiebreak would be non-
         * deterministic. The flat `w` is globally unique across the whole view
         * AND deterministic (chunkWork is built by iterating the
         * deterministically-sorted archetypes x their chunks in order). The
         * per-archetype chunk index is still what selects the actual chunk.
         *
         * Additive sibling of ParallelForEach: REUSES ParallelForEachChunkImpl
         * for the chunk walk (both the no-optional ForEachChunk path and the
         * optional InvokeEntityCallback path invoke the callback as
         * `(entity, component-refs..., [optional ptrs...])`, which the wrapper
         * adapts by appending `sub`). Does NOT modify ParallelForEach itself.
         *
         * Unlike ParallelForEach, the null-scheduler / below-threshold case does
         * NOT delegate to ForEach (which has neither a per-chunk sub-context nor
         * a chunk index): it builds chunkWork unconditionally and walks it INLINE
         * IN FLAT ORDER, which is deterministic. The per-chunk work is factored
         * into one `runChunk(w)` local used by both the scheduler-dispatch and
         * the inline path so the two can't drift.
         *
         * RETURNS the number of chunk-work items processed (chunkWork.size()) --
         * i.e. the count of distinct flat indices `w` in [0, chunkWork.size())
         * the factory could have been called with, IDENTICAL on the scheduler-
         * dispatch and inline paths (both process the whole chunkWork). Every
         * early-return path returns 0. SystemContext::ParallelForEach uses this
         * to advance its per-scope iterationIndex band by exactly the number of
         * distinct iterationIndex values this call could have stamped, keeping
         * deferred-command SortKeys globally unique across sequential calls.
         * Callers that ignore the return value are unaffected (additive).
         *
         * Change detection (spec §3.4): a change-filtered view needs a "since"
         * tick -- SystemContext::ParallelForEach calls the Tick-taking overload
         * below; this one is compile-time refused on such a view.
         */
        template<typename Factory, typename Body>
        ASTRA_FORCEINLINE size_t ParallelForEachWithContext(Factory&& factory, Body&& body)
        {
            static_assert(!HasChangeFilter,
                "This view has a Changed<T>/Added<T> filter and needs a 'since' tick: call "
                "ParallelForEachWithContext(since, factory, body) (SystemContext::ParallelForEach does).");
            return ParallelForEachWithContext(Tick{0}, std::forward<Factory>(factory), std::forward<Body>(body));
        }

        /**
         * ParallelForEachWithContext with an explicit "since" tick for the view's
         * Changed<T>/Added<T> terms (ignored by an unfiltered view). Same contract
         * and return value as the two-argument form above.
         */
        template<typename Factory, typename Body>
        ASTRA_FORCEINLINE size_t ParallelForEachWithContext(Tick since, Factory&& factory, Body&& body)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return 0;  // Registry destroyed

            EnsureArchetypes();

            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return 0;

            size_t quickCount = 0;
            for (Archetype* archetype : m_archetypes)
            {
                quickCount += archetype->GetEntityCount();
            }

            // Build the chunk work list unconditionally (see method doc): both
            // the parallel-dispatch and the inline fallback path need it, and
            // the per-archetype chunk index it carries.
            std::vector<std::pair<Archetype*, size_t>> chunkWork;
            size_t estimatedChunks = (quickCount / AVG_ENTITIES_PER_CHUNK) + m_archetypes.size();
            chunkWork.reserve(estimatedChunks);
            size_t totalMatchingEntities = 0;

            for (Archetype* archetype : m_archetypes)
            {
                size_t chunkCount = archetype->GetChunkCount();
                for (size_t i = 0; i < chunkCount; ++i)
                {
                    size_t chunkEntityCount = archetype->GetChunkEntityCount(i);
                    if (chunkEntityCount > 0)
                    {
                        chunkWork.emplace_back(archetype, i);
                        totalMatchingEntities += chunkEntityCount;
                    }
                }
            }

            if (chunkWork.empty()) ASTRA_UNLIKELY
                return 0;

            // Per-chunk work, shared by the scheduler-dispatch and inline paths
            // so they can't drift. `w` is the FLAT chunkWork index -- the
            // iterationIndex stamped into the sub-context; chunkWork[w].second is
            // the per-archetype chunk index selecting the actual chunk.
            auto runChunk = [&](size_t w)
            {
                auto [archetype, chunkIndex] = chunkWork[w];
                auto sub = factory(static_cast<uint32_t>(w));
                auto wrapped = [&body, &sub](Astra::Entity e, auto&&... comps)
                {
                    body(e, std::forward<decltype(comps)>(comps)..., sub);
                };
                ParallelForEachChunkImpl(archetype, chunkIndex, wrapped, since, RequiredTypes{}, OptionalTypes{});
            };

            // No scheduler, or workload below the parallel thresholds: walk every
            // chunk inline, in flat order -- deterministic, no thread fan-out.
            if (!m_scheduler ||
                quickCount < MIN_ENTITIES_QUICK_CHECK ||
                totalMatchingEntities < MIN_ENTITIES_FOR_PARALLEL ||
                chunkWork.size() < MIN_CHUNKS_FOR_PARALLEL)
            {
                for (size_t w = 0; w < chunkWork.size(); ++w)
                {
                    runChunk(w);
                }
                return chunkWork.size();
            }

            m_scheduler->ParallelFor(chunkWork.size(), MIN_CHUNKS_PER_THREAD,
                [&](size_t begin, size_t end, uint32_t /*worker*/)
                {
                    for (size_t w = begin; w < end; ++w)
                    {
                        runChunk(w);
                    }
                });
            return chunkWork.size();
        }

        ASTRA_NODISCARD size_t Size() noexcept
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return 0;  // Registry destroyed

            EnsureArchetypes();

            size_t total = 0;
            if constexpr (!HasRequiredFilter)
            {
                // No required enableable filter: entity count is unaffected by
                // disabled bits (optional filtering never removes entities), so
                // this is the pre-existing sum (invariant 1).
                for (Archetype* archetype : m_archetypes)
                {
                    total += archetype->GetEntityCount();
                }
            }
            else
            {
                for (Archetype* archetype : m_archetypes)
                {
                    total += SizeFiltered(archetype);
                }
            }
            return total;
        }

        ASTRA_NODISCARD bool Empty() noexcept
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            return Size() == 0;
        }

        ASTRA_NODISCARD bool Contains(Entity e) const
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            return VisibleRecord(e) != nullptr;
        }

        // ============= Range-based for loop support =============

        /**
         * Iterator type for range-based for loops.
         * Uses the required components from the query.
         */
        using Iterator = ViewIteratorFromTuple_t<RequiredTypes>;

        /**
         * Begin iterator for range-based for loop.
         * Ensures archetypes are up-to-date before returning iterator.
         */
        ASTRA_FORCEINLINE Iterator begin()
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            // Ruling I: ViewIterator yields plain T&, which cannot mark a change-
            // tracked entity's `changed` tick -- every write through range-for would
            // be an exact-tier false negative. Refuse at compile time (same precedent
            // as the enableable filter below); teaching the iterator to yield Mut<T>
            // is a recorded follow-up.
            static_assert(!HasTrackedYield,
                "range-for over a change-tracked component requested non-const is not supported: the iterator yields T& "
                "and cannot mark the entity's changed tick. Use ForEach(fn) (yields Mut<T>), or request `const T` for a read-only walk.");
            // Range-based for cannot honor the enableable disabled-bit filter:
            // ViewIterator has no access to the per-chunk disabled words, so
            // `for (auto x : view)` would visit disabled entities that
            // ForEach()/Size() skip -- the two surfaces would silently disagree
            // (IM-7). Refuse at compile time when a REQUIRED enableable filter is
            // active; the body of this non-template member is only instantiated
            // when begin() is actually used, so ForEach()/Size()-only usage of
            // such a View still compiles. Optional-only enableable filters are
            // NOT gated: range-for never yields optional components and the
            // visited entity set is identical to ForEach()'s, so they do not
            // diverge.
            static_assert(!HasRequiredFilter,
                "Range-based for over a View with a required enableable-component filter is not "
                "supported: it would bypass disabled-bit filtering and disagree with ForEach()/Size(). "
                "Use ForEach() or ParallelForEach() instead.");

            if (!m_archetypeManager) ASTRA_UNLIKELY
                return Iterator{};   // empty iterator: no archetypes, enters (and stamps) no chunk

            EnsureArchetypes();
            // The iterator carries the current tick so it can apply the coarse
            // change-detection stamp on every chunk it enters (non-const yields only).
            return Iterator(m_archetypes.data(), m_archetypes.size(), m_archetypeManager->CurrentTick());
        }

        /**
         * End sentinel for range-based for loop.
         */
        ASTRA_FORCEINLINE ViewSentinel end() const noexcept
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            // See begin() (Ruling I): the iterator cannot mark a tracked entity.
            static_assert(!HasTrackedYield,
                "range-for over a change-tracked component requested non-const is not supported: the iterator yields T& "
                "and cannot mark the entity's changed tick. Use ForEach(fn) (yields Mut<T>), or request `const T` for a read-only walk.");
            // See begin(): range-for is compile-time refused on required
            // enableable-filtered views so it cannot silently diverge from
            // ForEach()/Size() (IM-7).
            static_assert(!HasRequiredFilter,
                "Range-based for over a View with a required enableable-component filter is not "
                "supported: it would bypass disabled-bit filtering and disagree with ForEach()/Size(). "
                "Use ForEach() or ParallelForEach() instead.");
            return ViewSentinel{};
        }

    private:
        // Random-access yield shape: each required -> a pointer (const preserved),
        // or a by-value Mut<R> for a non-const change-tracked R; each optional -> a
        // pointer. Mirrors ForEach's yielded arguments.
        template<typename R>
        using AccessElement = std::conditional_t<Detail::IsMutableYield<R> && IsChangeTrackedV<R>, Mut<R>, R*>;

        template<typename ReqTuple, typename OptTuple> struct AccessTupleImpl;
        template<typename... R, typename... O>
        struct AccessTupleImpl<std::tuple<R...>, std::tuple<O...>>
        {
            using type = std::tuple<AccessElement<R>..., O*...>;
        };

        template<typename R>
        ASTRA_FORCEINLINE AccessElement<R> BindRequired(const EntityRecord* rec) const
        {
            using Bare = std::remove_const_t<R>;
            if constexpr (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
            {
                // Handing out the Mut does not mark; its first mutable use does, with
                // the tick current at this call.
                const int col = rec->archetype->GetColumnMeta().idToColumn[TypeID<Bare>::Value()];
                return Mut<R>(rec->archetype->template GetComponent<Bare>(rec->location),
                              rec->chunk->GetTicks(col) + rec->location.GetEntityIndex(),
                              m_archetypeManager->CurrentTick());
            }
            else
            {
                return rec->archetype->template GetComponent<Bare>(rec->location);   // Bare* -> R* (adds const if any)
            }
        }
        template<typename O>
        ASTRA_FORCEINLINE O* BindOptional(const EntityRecord* rec) const
        {
            using Bare = std::remove_const_t<O>;
            if (!rec->archetype->template HasComponent<Bare>())
                return nullptr;
            if constexpr (IsEnableableV<Bare>)
            {
                const ArchetypeColumnMeta& cm = rec->archetype->GetColumnMeta();
                if (rec->chunk->IsDisabled(cm.idToColumn[TypeID<Bare>::Value()], rec->location.GetEntityIndex()))
                    return nullptr;
            }
            if constexpr (Detail::IsMutableYield<O> && IsChangeTrackedV<O>)
            {
                // Plan deviation 5: a present non-const tracked optional is a raw O*,
                // so it marks the entity unconditionally (see MarkTrackedOptional).
                const int col = rec->archetype->GetColumnMeta().idToColumn[TypeID<Bare>::Value()];
                rec->chunk->GetTicks(col)[rec->location.GetEntityIndex()].changed = m_archetypeManager->CurrentTick();
            }
            return rec->archetype->template GetComponent<Bare>(rec->location);
        }

        template<typename... R, typename... O, size_t... Ri, size_t... Oi>
        ASTRA_FORCEINLINE auto MakeAccessTuple(const EntityRecord* rec,
                                               std::tuple<R...>, std::tuple<O...>,
                                               std::index_sequence<Ri...>, std::index_sequence<Oi...>) const
        {
            return typename AccessTupleImpl<RequiredTypes, OptionalTypes>::type{
                BindRequired<std::tuple_element_t<Ri, RequiredTypes>>(rec)...,
                BindOptional<std::tuple_element_t<Oi, OptionalTypes>>(rec)...
            };
        }

        // Get()'s stamp: the optional-presence array is built from the entity's own
        // archetype, then the shared per-chunk stamp is applied to its chunk.
        template<size_t... Oi>
        ASTRA_FORCEINLINE void StampForGet(const EntityRecord* rec, std::index_sequence<Oi...> optSeq) const
        {
            std::array<bool, sizeof...(Oi)> hasOptional =
            {
                rec->archetype->template HasComponent<std::tuple_element_t<Oi, OptionalTypes>>()...
            };
            StampChunkForYields(rec->chunk, rec->archetype->GetColumnMeta(), hasOptional, m_archetypeManager->CurrentTick(),
                                std::make_index_sequence<std::tuple_size_v<RequiredTypes>>{}, optSeq);
        }

    public:
        using AccessTuple = typename AccessTupleImpl<RequiredTypes, OptionalTypes>::type;

        /**
         * Filter-aware random access: returns pointers to the yielded components for
         * `e` (required -> T*, or Mut<T> for a non-const change-tracked T; Optional ->
         * T*), or Err(NotMatched) if `e` is absent, dead, filtered out, or disabled
         * under this view's enabled filter. The pointers point INTO live chunk storage
         * and are invalidated by any structural change (create/destroy/add/remove/
         * defragment) — do not retain them across one. In-place value edits through
         * the pointers are fine.
         *
         * Change detection: a NON-CONST requested component stamps the entity's chunk
         * (that column's coarse version) on the way out, even on a `const View` -- and
         * a change-tracked one is handed out as Mut<T>, which marks the entity on use.
         * The Mut<T> carries the tick current when Get() ran: a handle retained across
         * AdvanceTick() marks with that older tick, so do not keep one across frames
         * (mark later writes with Registry::Modified<T>). All-const requests stamp
         * nothing.
         */
        ASTRA_NODISCARD Result<AccessTuple, QueryError> Get(Entity e) const
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            const EntityRecord* rec = VisibleRecord(e);
            if (!rec) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::NotMatched);
            // Coarse change-detection stamp (spec §3.3): a non-const request is a
            // write, so stamp the entity's chunk for every non-const yielded column
            // that is present. All-const request: compiles to nothing.
            StampForGet(rec, std::make_index_sequence<std::tuple_size_v<OptionalTypes>>{});
            return Result<AccessTuple, QueryError>::Ok(
                MakeAccessTuple(rec, RequiredTypes{}, OptionalTypes{},
                                std::make_index_sequence<std::tuple_size_v<RequiredTypes>>{},
                                std::make_index_sequence<std::tuple_size_v<OptionalTypes>>{}));
        }

        /**
         * Filter-aware exactly-one-match accessor: Err(Empty) if no entity
         * matches this view, Err(MultipleMatched) if more than one does,
         * otherwise Ok(Get(the one match)). Non-const because it reuses the
         * ForEach walk (which calls EnsureArchetypes()).
         *
         * Change detection (spec §3.3 row 2, Ruling E): a non-const requested
         * component stamps the returned entity's chunk (and a change-tracked one
         * comes back as Mut<T>, marking on use) -- exactly Get()'s behaviour, because
         * the one match is materialised through Get(). Only the RETURNED entity's
         * chunk is stamped; the count/locate walk below runs UNSTAMPED
         * (ForEachBody<false>) because counting is not a write -- so nothing is
         * marked on the Empty / MultipleMatched paths, and no chunk other than
         * the returned entity's is marked on success.
         */
        ASTRA_NODISCARD Result<AccessTuple, QueryError> Single()
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            Entity found{};
            size_t count = 0;
            // Tick{0} is inert: Single() is refused above on change-filtered views.
            ForEachBody<false>(Tick{0}, [&](Entity e, auto&&...) { if (count == 0) found = e; ++count; });
            if (count == 0) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::Empty);
            if (count > 1) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::MultipleMatched);
            return Get(found);   // exactly one visible match; Get re-validates, materializes AND stamps its chunk
        }

    private:
        // The record iff `e` is alive AND structurally matches this view AND is
        // enabled-visible; else nullptr. No EnsureArchetypes needed — matching is
        // tested against the entity's OWN archetype mask.
        ASTRA_NODISCARD const EntityRecord* VisibleRecord(Entity e) const
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY return nullptr;
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
            if (!rec) return nullptr;   // dead/absent (GetEntityRecord checks version + archetype)
            if (!QueryBuilder::Matches(rec->archetype->GetMask())) return nullptr;
            if constexpr (HasRequiredFilter)
            {
                if (!EnabledVisible(rec)) return nullptr;
            }
            return rec;
        }

        // False iff any required enableable component is DISABLED for this entity.
        // Only instantiated when HasRequiredFilter (guarded at the call site).
        ASTRA_NODISCARD bool EnabledVisible(const EntityRecord* rec) const
        {
            return EnabledVisibleImpl(rec, EnabledRequiredFilter{});
        }
        template<typename... Fs>
        ASTRA_NODISCARD bool EnabledVisibleImpl(const EntityRecord* rec, std::tuple<Fs...>) const
        {
            const ArchetypeColumnMeta& cm = rec->archetype->GetColumnMeta();
            const size_t idx = rec->location.GetEntityIndex();
            bool disabled = false;
            ((disabled = disabled || rec->chunk->IsDisabled(cm.idToColumn[TypeID<Fs>::Value()], idx)), ...);
            return !disabled;
        }

        struct ArchetypeEntityCountComparator
        {
            bool operator()(Archetype* a, Archetype* b) const
            {
                return a->GetEntityCount() > b->GetEntityCount();
            }
        };

        void EnsureArchetypes()
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            uint32_t currentCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
            if (m_lastRefreshCounter == currentCounter)
            {
                return;
            }

            uint32_t removalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
            if (m_lastGeneration == 0 || removalCounter != m_lastRemovalCounter)
            {
                // Archetypes were removed (or first refresh): cached pointers
                // may be stale — rebuild the whole list.
                CollectArchetypes();
                m_lastRemovalCounter = removalCounter;
            }
            else
            {
                auto newArchetypes = m_archetypeManager->GetArchetypesSince(m_lastGeneration);
                for (Archetype* arch : newArchetypes)
                {
                    if (QueryBuilder::Matches(arch->GetMask()))
                    {
                        m_archetypes.push_back(arch);
                    }
                }
                std::sort(m_archetypes.begin(), m_archetypes.end(), ArchetypeEntityCountComparator{});
            }

            m_lastRefreshCounter = currentCounter;
            m_lastGeneration = m_archetypeManager->m_generation;
        }

        void CollectArchetypes()
        {
            m_archetypes.clear();
            if (!m_archetypeManager) ASTRA_UNLIKELY
            {
                return;  // Registry destroyed
            }

            auto archetypes = m_archetypeManager->GetArchetypes();
            const size_t queryComponentCount =
                (QueryBuilder::GetRequiredMask() | QueryBuilder::GetWithMask() | QueryBuilder::GetChangeMask()).Count();

            m_archetypes.reserve(archetypes.size());

            for (Archetype* archetype : archetypes)
            {
                // NOTE: empty archetypes are deliberately KEPT — they may gain
                // entities later without any archetype-creation event, and
                // iterating an empty archetype is free (zero-count chunks).
                if (archetype->GetComponentCount() < queryComponentCount) ASTRA_UNLIKELY
                {
                    continue;
                }
                if (QueryBuilder::Matches(archetype->GetMask()))
                {
                    m_archetypes.push_back(archetype);
                }
            }

            std::sort(m_archetypes.begin(), m_archetypes.end(), ArchetypeEntityCountComparator{});
        }

        // Wrap a user callback so it can be invoked as (Entity, Comps&...): if the
        // callback also accepts the leading Entity, forward it; otherwise drop it.
        // Additive: an existing (Entity, Comps...) body hits the first branch and is
        // called identically. Resolves once per fixed component pack (per view type).
        template<typename Func>
        ASTRA_FORCEINLINE static auto MakeEntityOptionalAdapter(Func&& func)
        {
            return [&func](Astra::Entity e, auto&&... comps)
            {
                if constexpr (std::is_invocable_v<Func&, Astra::Entity, decltype(comps)...>)
                    func(e, std::forward<decltype(comps)>(comps)...);
                else if constexpr (std::is_invocable_v<Func&, decltype(comps)...>)
                    func(std::forward<decltype(comps)>(comps)...);
                else
                    static_assert(sizeof(Func) == 0,
                        "View callback must be invocable as (Entity, Comps&...) or (Comps&...). "
                        "Component params must be 'T&' (write) or 'const T&' (read); "
                        "Optional<T> supplies a 'T*' argument. "
                        "A change-tracked non-const component is yielded as Mut<T> (converts to T&; use .Read()/.Write()/.SetIfNeq()).");
            };
        }

        // Coarse change-detection stamp for a chunk this view is about to iterate:
        // every non-const required column, plus every non-const optional column that
        // is PRESENT on this archetype. Compiles to nothing for an all-const view:
        // the if constexpr gate below is the structural guarantee (no stamp code is
        // instantiated), same shape as the Archetype fast path's Stamp && AnyMutable.
        template<size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void StampChunkForYields(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm,
                                                   const std::array<bool, sizeof...(OptIs)>& hasOptional, Tick now,
                                                   std::index_sequence<ReqIs...>, std::index_sequence<OptIs...>) const
        {
            constexpr bool AnyMutable = (Detail::IsMutableYield<std::tuple_element_t<ReqIs, RequiredTypes>> || ...)
                                     || (Detail::IsMutableYield<std::tuple_element_t<OptIs, OptionalTypes>> || ...);
            if constexpr (AnyMutable)
            {
                ((Detail::IsMutableYield<std::tuple_element_t<ReqIs, RequiredTypes>>
                    ? chunk->StampColumn(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<ReqIs, RequiredTypes>>>::Value()], now)
                    : void()), ...);
                (((Detail::IsMutableYield<std::tuple_element_t<OptIs, OptionalTypes>> && hasOptional[OptIs])
                    ? chunk->StampColumn(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<OptIs, OptionalTypes>>>::Value()], now)
                    : void()), ...);
            }
        }

        // The one archetype walk behind ForEach (Stamp=true) and the internal
        // unstamped pass (Stamp=false: Single()'s count/locate, which is not a
        // write -- Ruling E / spec §3.3 row 2). Mirrors Archetype::ForEachBody so the
        // stamped and unstamped walks cannot drift; with Stamp=false no stamp code
        // is instantiated anywhere below. `since` is the change-filter tick
        // (Changed<T>/Added<T> chunk reject); ignored by a view without one.
        template<bool Stamp, typename Func>
        ASTRA_FORCEINLINE void ForEachBody(Tick since, Func&& func)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            EnsureArchetypes();

#ifdef ASTRA_BUILD_DEBUG
            // Captured AFTER EnsureArchetypes() so its own (legitimate) refresh
            // of the counter is never mistaken for an in-loop structural change.
            const uint32_t debugStartStructuralChangeCounter =
                m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
#endif

            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return;

            auto adapted = MakeEntityOptionalAdapter(func);
            for (Archetype* archetype : m_archetypes)
            {
                ForEachImpl<Stamp>(archetype, adapted, since, RequiredTypes{}, OptionalTypes{});
            }

#ifdef ASTRA_BUILD_DEBUG
            ASTRA_ASSERT(m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire) == debugStartStructuralChangeCounter,
                "Structural mutation (create/destroy entity, add/remove component) detected during View::ForEach. "
                "Defer structural changes into a CommandBuffer and call Execute() after the loop.");
#endif
        }

        template<bool Stamp, typename Func, typename... Required, typename... Optional>
        ASTRA_FORCEINLINE void ForEachImpl(Archetype* archetype, Func&& func, Tick since, std::tuple<Required...>, std::tuple<Optional...>)
        {
            if constexpr (!HasChunkFilter)
            {
                // No enableable (non-IncludeDisabled) type and no Changed/Added term
                // in the query: the filtered path below is not instantiated at all,
                // so this is the pre-existing loop (invariant 1) plus, when Stamp,
                // the coarse change-detection stamp of every non-const yielded
                // column per visited chunk (all-const: no stamp code). A tracked
                // non-const yield (Mut<T>) needs the view-owned loop below, which
                // already handles zero optionals.
                (void)since;
                if constexpr (sizeof...(Optional) == 0 && !HasTrackedYield)
                {
                    if constexpr (Stamp)
                        archetype->ForEachStamped<Required...>(std::forward<Func>(func));
                    else
                        archetype->ForEach<Required...>(std::forward<Func>(func));
                }
                else
                {
                    ForEachWithOptional<Stamp, Required..., Optional...>(archetype, std::forward<Func>(func), std::make_index_sequence<sizeof...(Required)>{}, std::make_index_sequence<sizeof...(Optional)>{});
                }
            }
            else
            {
                const Tick now = m_archetypeManager->CurrentTick();
                const auto& chunks = archetype->GetChunks();
                for (auto& chunk : chunks)
                {
                    VisitChunkFiltered<Stamp>(archetype, chunk.get(), func, now, since,
                                              std::make_index_sequence<sizeof...(Required)>{},
                                              std::make_index_sequence<sizeof...(Optional)>{});
                }
            }
        }
        
        template<bool Stamp, typename... Components, typename Func, size_t... RequiredTs, size_t... OptionalTs>
        ASTRA_FORCEINLINE void ForEachWithOptional(Archetype* archetype, Func&& func, std::index_sequence<RequiredTs...>, std::index_sequence<OptionalTs...>)
        {
            constexpr size_t OptionalCount = sizeof...(OptionalTs);
            std::array<bool, OptionalCount> hasOptional =
            {
                archetype->HasComponent<std::tuple_element_t<OptionalTs, OptionalTypes>>()...
            };

            [[maybe_unused]] const Tick now = m_archetypeManager->CurrentTick();
            [[maybe_unused]] const auto& cm = archetype->GetColumnMeta();
            const auto& chunks = archetype->GetChunks();

            for (auto& chunk : chunks)
            {
                size_t count = chunk->GetCount();
                if (count == 0) ASTRA_UNLIKELY
                {
                    continue;
                }

                if constexpr (Stamp)
                    StampChunkForYields(chunk.get(), cm, hasOptional, now,
                                        std::index_sequence<RequiredTs...>{}, std::index_sequence<OptionalTs...>{});

                std::tuple<std::tuple_element_t<RequiredTs, RequiredTypes>*...> requiredPtrs =
                {
                    chunk->GetComponentArray<std::tuple_element_t<RequiredTs, RequiredTypes>>()...
                };
                std::tuple<std::tuple_element_t<OptionalTs, OptionalTypes>*...> optionalPtrs =
                {
                    (hasOptional[OptionalTs] ? chunk->GetComponentArray<std::tuple_element_t<OptionalTs, OptionalTypes>>() : nullptr)...
                };

                // Tick columns for Mut<T> / optional marks (tracked yields only).
                [[maybe_unused]] EntityTicks* reqTicks[sizeof...(RequiredTs) == 0 ? 1 : sizeof...(RequiredTs)] = {};
                [[maybe_unused]] EntityTicks* optTicks[sizeof...(OptionalTs) == 0 ? 1 : sizeof...(OptionalTs)] = {};
                if constexpr (HasTrackedYield)
                    ResolveTrackedTicks(chunk.get(), cm, hasOptional, reqTicks, optTicks,
                                        std::index_sequence<RequiredTs...>{}, std::index_sequence<OptionalTs...>{});

                const auto& entities = chunk->GetEntities();

                InvokeEntityCallback<Stamp>(entities, requiredPtrs, optionalPtrs, count, std::forward<Func>(func), std::make_index_sequence<sizeof...(RequiredTs)>{}, std::make_index_sequence<sizeof...(OptionalTs)>{},
                                            reqTicks, optTicks, now);
            }
        }

        template<typename Func, typename... Required, typename... Optional>
        ASTRA_FORCEINLINE void ParallelForEachChunkImpl(Archetype* archetype, size_t chunkIndex, Func&& func, Tick since, std::tuple<Required...>, std::tuple<Optional...>)
        {
            if constexpr (!HasChunkFilter)
            {
                // Pre-existing chunk walk (invariant 1) plus the coarse change-
                // detection stamp of every non-const yielded column for this chunk.
                // A tracked non-const yield (Mut<T>) takes the view-owned loop.
                (void)since;
                if constexpr (sizeof...(Optional) == 0 && !HasTrackedYield)
                {
                    archetype->ForEachChunkStamped<Required...>(chunkIndex, std::forward<Func>(func));
                }
                else
                {
                    ParallelForEachChunkWithOptional<Required..., Optional...>(archetype, chunkIndex, std::forward<Func>(func), std::make_index_sequence<sizeof...(Required)>{}, std::make_index_sequence<sizeof...(Optional)>{});
                }
            }
            else
            {
                // Same per-chunk three-tier filter as the serial path; partitioning
                // (which chunk this worker got) is unchanged.
                const auto& chunks = archetype->GetChunks();
                if (chunkIndex >= chunks.size()) ASTRA_UNLIKELY
                    return;
                VisitChunkFiltered<true>(archetype, chunks[chunkIndex].get(), func, m_archetypeManager->CurrentTick(), since,
                                         std::make_index_sequence<sizeof...(Required)>{},
                                         std::make_index_sequence<sizeof...(Optional)>{});
            }
        }
        
        template<typename... Components, typename Func, size_t... RequiredTs, size_t... OptionalTs>
        ASTRA_FORCEINLINE void ParallelForEachChunkWithOptional(Archetype* archetype, size_t chunkIndex, Func&& func, std::index_sequence<RequiredTs...>, std::index_sequence<OptionalTs...>)
        {
            constexpr size_t OptionalCount = sizeof...(OptionalTs);
            std::array<bool, OptionalCount> hasOptional =
            {
                archetype->HasComponent<std::tuple_element_t<OptionalTs, OptionalTypes>>()...
            };
            
            const auto& chunks = archetype->GetChunks();
            if (chunkIndex >= chunks.size()) ASTRA_UNLIKELY
                return;

            auto& chunk = chunks[chunkIndex];
            size_t count = chunk->GetCount();
            if (count == 0) ASTRA_UNLIKELY
                return;

            // One tick read per chunk: the coarse stamp and the Mut<T> marks agree.
            [[maybe_unused]] const Tick now = m_archetypeManager->CurrentTick();
            [[maybe_unused]] const auto& cm = archetype->GetColumnMeta();

            StampChunkForYields(chunk.get(), cm, hasOptional, now,
                                std::index_sequence<RequiredTs...>{}, std::index_sequence<OptionalTs...>{});

            std::tuple<std::tuple_element_t<RequiredTs, RequiredTypes>*...> requiredPtrs =
            {
                chunk->GetComponentArray<std::tuple_element_t<RequiredTs, RequiredTypes>>()...
            };
            std::tuple<std::tuple_element_t<OptionalTs, OptionalTypes>*...> optionalPtrs =
            {
                (hasOptional[OptionalTs] ? chunk->GetComponentArray<std::tuple_element_t<OptionalTs, OptionalTypes>>() : nullptr)...
            };

            // Tick columns for Mut<T> / optional marks (tracked yields only).
            [[maybe_unused]] EntityTicks* reqTicks[sizeof...(RequiredTs) == 0 ? 1 : sizeof...(RequiredTs)] = {};
            [[maybe_unused]] EntityTicks* optTicks[sizeof...(OptionalTs) == 0 ? 1 : sizeof...(OptionalTs)] = {};
            if constexpr (HasTrackedYield)
                ResolveTrackedTicks(chunk.get(), cm, hasOptional, reqTicks, optTicks,
                                    std::index_sequence<RequiredTs...>{}, std::index_sequence<OptionalTs...>{});

            const auto& entities = chunk->GetEntities();

            InvokeEntityCallback<true>(entities, requiredPtrs, optionalPtrs, count, std::forward<Func>(func), std::make_index_sequence<sizeof...(RequiredTs)>{}, std::make_index_sequence<sizeof...(OptionalTs)>{},
                                       reqTicks, optTicks, now);
        }

        // Empty (tag) required components have no storage, so
        // chunk->GetComponentArray<T>() returns nullptr for them. Indexing that
        // raw pointer (`ptr[i]`) would bind a reference to a null-derived address
        // (UB; UBSan traps). Mirror the plain path's Archetype::GetComponentValue
        // and hand out a shared static instance instead. For non-empty components
        // this force-inlines to the identical `array[index]`, so the hot path is
        // unchanged.
        template<typename T>
        ASTRA_FORCEINLINE static auto& RequiredElement(T* array, size_t index) noexcept
        {
            if constexpr (std::is_empty_v<T>)
            {
                static T s_emptyInstance{};
                return s_emptyInstance;
            }
            else
            {
                return array[index];
            }
        }

        // ============ Change-tracked yield (spec §3.3, instantiated only when HasTrackedYield) ============
        // Everything below the untracked branch of the two invoke helpers exists ONLY
        // for a view that yields a non-const change-tracked component; every other
        // view instantiates the pre-existing loop body byte for byte.

        // Yield shape of one required component: Mut<R> for a non-const change-
        // tracked R, otherwise the plain reference the pre-existing loops hand out.
        template<typename R> struct YieldType { using type = R&; };
        template<typename R> requires (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
        struct YieldType<R> { using type = Mut<R>; };

        // reqTicks[k] is the tick column for required k (nullptr unless tracked).
        template<size_t K, typename ReqTuple>
        ASTRA_FORCEINLINE typename YieldType<std::tuple_element_t<K, RequiredTypes>>::type
        YieldRequired(const ReqTuple& reqPtrs, [[maybe_unused]] EntityTicks* const* reqTicks, size_t i, [[maybe_unused]] Tick now) const noexcept
        {
            using R = std::tuple_element_t<K, RequiredTypes>;
            if constexpr (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
                return Mut<R>(&std::get<K>(reqPtrs)[i], reqTicks[K] + i, now);
            else
                return RequiredElement(std::get<K>(reqPtrs), i);
        }

        // Plan deviation 5: a PRESENT non-const change-tracked optional is yielded as
        // a raw O* (no conversion hook to mark through), so it marks the entity
        // unconditionally. `opt` holds the pointers about to be handed out for entity
        // i -- the chunk base pointers on the unfiltered path, the per-entity pointers
        // on the enabled-filtered path (a disabled entity's pointer is nulled there
        // and is not "present", so it must not mark).
        template<size_t K, typename OptTuple>
        ASTRA_FORCEINLINE static void MarkTrackedOptional([[maybe_unused]] const OptTuple& opt, [[maybe_unused]] EntityTicks* const* optTicks,
                                                          [[maybe_unused]] size_t i, [[maybe_unused]] Tick now) noexcept
        {
            using O = std::tuple_element_t<K, OptionalTypes>;
            if constexpr (Detail::IsMutableYield<O> && IsChangeTrackedV<O>)
            {
                if (std::get<K>(opt)) optTicks[K][i].changed = now;
            }
        }
        template<typename OptTuple, size_t... OptIs>
        ASTRA_FORCEINLINE static void MarkTrackedOptionals([[maybe_unused]] const OptTuple& opt, [[maybe_unused]] EntityTicks* const* optTicks,
                                                           [[maybe_unused]] size_t i, [[maybe_unused]] Tick now, std::index_sequence<OptIs...>) noexcept
        {
            (MarkTrackedOptional<OptIs>(opt, optTicks, i, now), ...);
        }

        // Tick columns for the tracked yields of one chunk: reqTicks[k] / optTicks[k]
        // is the EntityTicks column for required / optional k, nullptr unless that
        // yield is non-const AND change-tracked (AND, for an optional, present on this
        // archetype). Same fold shape as StampChunkForYields; called only under
        // `if constexpr (HasTrackedYield)`.
        template<size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE static void ResolveTrackedTicks([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                                          [[maybe_unused]] const std::array<bool, sizeof...(OptIs)>& hasOptional,
                                                          [[maybe_unused]] EntityTicks** reqTicks, [[maybe_unused]] EntityTicks** optTicks,
                                                          std::index_sequence<ReqIs...>, std::index_sequence<OptIs...>) noexcept
        {
            ((reqTicks[ReqIs] = (Detail::IsMutableYield<std::tuple_element_t<ReqIs, RequiredTypes>> && IsChangeTrackedV<std::tuple_element_t<ReqIs, RequiredTypes>>)
                ? chunk->GetTicks(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<ReqIs, RequiredTypes>>>::Value()]) : nullptr), ...);
            ((optTicks[OptIs] = (hasOptional[OptIs] && Detail::IsMutableYield<std::tuple_element_t<OptIs, OptionalTypes>> && IsChangeTrackedV<std::tuple_element_t<OptIs, OptionalTypes>>)
                ? chunk->GetTicks(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<OptIs, OptionalTypes>>>::Value()]) : nullptr), ...);
        }

        // `Stamp` mirrors the chunk loops' parameter: with Stamp=false (Single()'s
        // count/locate pass -- Ruling E, extended to the exact tier) the deviation-5
        // optional mark is not instantiated either; a Mut<T> handed to the counting
        // lambda is never touched, so required yields need no gate.
        template<bool Stamp, typename EntitiesVec, typename ReqTuple, typename OptTuple, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void InvokeEntityCallback(const EntitiesVec& entities, const ReqTuple& reqPtrs, const OptTuple& optPtrs,
                                                    size_t count, Func&& func, std::index_sequence<ReqIs...>, [[maybe_unused]] std::index_sequence<OptIs...> os,
                                                    [[maybe_unused]] EntityTicks* const* reqTicks = nullptr, [[maybe_unused]] EntityTicks* const* optTicks = nullptr,
                                                    [[maybe_unused]] Tick now = 0)
        {
            if constexpr (!HasTrackedYield)
            {
                // Pre-existing body, byte-identical: every untracked view lands here.
                for (size_t i = 0; i < count; ++i)
                {
                    func(entities[i], RequiredElement(std::get<ReqIs>(reqPtrs), i)..., (std::get<OptIs>(optPtrs) ? &std::get<OptIs>(optPtrs)[i] : nullptr)...);
                }
            }
            else
            {
                for (size_t i = 0; i < count; ++i)
                {
                    if constexpr (Stamp)
                        MarkTrackedOptionals(optPtrs, optTicks, i, now, os);
                    // Materialise the yields as lvalues (a tuple of T&... / Mut<T>...) so
                    // `auto&`, `T&` (Mut's implicit conversion) and by-value `Mut<T>`
                    // parameters all bind; std::apply hands them over as T& / Mut<T>&.
                    std::tuple<typename YieldType<std::tuple_element_t<ReqIs, RequiredTypes>>::type...> req{ YieldRequired<ReqIs>(reqPtrs, reqTicks, i, now)... };
                    std::apply([&](auto&... r) { func(entities[i], r..., (std::get<OptIs>(optPtrs) ? &std::get<OptIs>(optPtrs)[i] : nullptr)...); }, req);
                }
            }
        }

        // ============ Enableable-components filtered iteration (spec §5) ============
        // Everything below is instantiated ONLY when HasEnabledFilter is true (the
        // callers gate it with `if constexpr`); an unfiltered view never sees it.

        // Resolve one required enableable-filtered column: record its disabled-word
        // pointer and fold its all-enabled / all-disabled state into the flags.
        template<size_t F>
        ASTRA_FORCEINLINE void ResolveOneRequired(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, size_t count,
                                                  const uint64_t** reqWords, bool& allZero, bool& anyFull)
        {
            using T = std::tuple_element_t<F, EnabledRequiredFilter>;
            const int col = cm.idToColumn[TypeID<T>::Value()];  // required => present, enableable => has words
            reqWords[F] = chunk->GetDisabledWords(col);
            const uint32_t dc = chunk->GetDisabledCount(col);
            if (dc == static_cast<uint32_t>(count)) anyFull = true;   // fully disabled => empty intersection
            if (dc != 0) allZero = false;
        }

        // The three pack-fed parameters are [[maybe_unused]]: with an empty Fs
        // pack the fold is a no-op and gcc's -Wunused-but-set-parameter fires.
        template<size_t... Fs>
        ASTRA_FORCEINLINE bool ResolveRequiredFilter([[maybe_unused]] ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm,
                                                     [[maybe_unused]] size_t count, [[maybe_unused]] const uint64_t** reqWords,
                                                     bool& allZero, std::index_sequence<Fs...>)
        {
            bool anyFull = false;
            allZero = true;
            (ResolveOneRequired<Fs>(chunk, cm, count, reqWords, allZero, anyFull), ...);
            return anyFull;
        }

        // A present, enableable optional with any disabled entity forces the mixed
        // path so its per-entity pointer can be nulled.
        template<size_t K, typename OptTuple>
        ASTRA_FORCEINLINE void CheckOptionalAllEnabled(const OptTuple& optPtrs, ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, bool& allZero)
        {
            using OptT = std::tuple_element_t<K, OptionalTypes>;
            if constexpr (IsEnableableV<OptT>)
            {
                if (std::get<K>(optPtrs) && chunk->GetDisabledCount(cm.idToColumn[TypeID<OptT>::Value()]) != 0)
                    allZero = false;
            }
        }

        // Per-entity optional pointer for the mixed path: null while the entity is
        // disabled in an enableable optional column; otherwise the usual present/
        // absent pointer. The bit test exists ONLY for enableable optionals (the
        // "second if constexpr"), so required-only / non-enableable-optional views
        // pay nothing.
        template<size_t K, typename OptTuple>
        ASTRA_FORCEINLINE auto FilteredOptionalArg(const OptTuple& optPtrs, size_t i, ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm)
        {
            using OptT = std::tuple_element_t<K, OptionalTypes>;
            OptT* base = std::get<K>(optPtrs);
            if constexpr (IsEnableableV<OptT>)
            {
                if (base && chunk->IsDisabled(cm.idToColumn[TypeID<OptT>::Value()], i))
                    return static_cast<OptT*>(nullptr);
            }
            return base ? &base[i] : static_cast<OptT*>(nullptr);
        }

        // Same `Stamp` gate as InvokeEntityCallback (Ruling E, extended).
        template<bool Stamp, typename EntitiesVec, typename ReqTuple, typename OptTuple, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void InvokeEntityCallbackFiltered(const EntitiesVec& entities, const ReqTuple& reqPtrs, const OptTuple& optPtrs,
                                                            size_t begin, size_t end, ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm,
                                                            Func&& func, std::index_sequence<ReqIs...>, [[maybe_unused]] std::index_sequence<OptIs...> os,
                                                            [[maybe_unused]] EntityTicks* const* reqTicks = nullptr, [[maybe_unused]] EntityTicks* const* optTicks = nullptr,
                                                            [[maybe_unused]] Tick now = 0)
        {
            if constexpr (!HasTrackedYield)
            {
                // Pre-existing body, byte-identical: every untracked view lands here.
                for (size_t i = begin; i < end; ++i)
                {
                    func(entities[i], RequiredElement(std::get<ReqIs>(reqPtrs), i)..., FilteredOptionalArg<OptIs>(optPtrs, i, chunk, cm)...);
                }
            }
            else
            {
                for (size_t i = begin; i < end; ++i)
                {
                    // Per-entity optional pointers first: an enableable tracked optional
                    // that is disabled for this entity is nulled here and must not mark.
                    std::tuple<std::tuple_element_t<OptIs, OptionalTypes>*...> opt{ FilteredOptionalArg<OptIs>(optPtrs, i, chunk, cm)... };
                    if constexpr (Stamp)
                        MarkTrackedOptionals(opt, optTicks, i, now, os);
                    std::tuple<typename YieldType<std::tuple_element_t<ReqIs, RequiredTypes>>::type...> req{ YieldRequired<ReqIs>(reqPtrs, reqTicks, i, now)... };
                    std::apply([&](auto&... r) { func(entities[i], r..., std::get<OptIs>(opt)...); }, req);
                }
            }
        }

        // ============ Change-detection chunk reject (spec §3.4, instantiated only when HasChangeFilter) ============

        // True iff every T in the tuple has a column version newer than `since` in
        // this chunk. Empty tuple => true (the fold's identity), which is why the
        // pack-fed parameters are [[maybe_unused]] (gcc -Wunused-but-set-parameter).
        template<typename... Ts>
        ASTRA_FORCEINLINE static bool AllNewer([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                               [[maybe_unused]] Tick since, std::tuple<Ts...>*) noexcept
        {
            return (IsNewer(chunk->GetColumnVersion(cm.idToColumn[TypeID<std::remove_const_t<Ts>>::Value()]), since) && ...);
        }
        ASTRA_FORCEINLINE static bool ChangeChunkPasses(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, Tick since) noexcept
        {
            return AllNewer(chunk, cm, since, static_cast<ChangedTypes*>(nullptr))
                && AllNewer(chunk, cm, since, static_cast<AddedTypes*>(nullptr));
        }

        // ============ Change-detection per-entity tier (spec §3.4 step 3, instantiated only when HasTrackedChangeTerm) ============

        // Per-entity tier (spec §3.4 step 3): for one tracked term, set the bit of
        // every entity whose tick is NOT newer than `since`. Branchless per slot; the
        // resulting word set is unioned with the enabled-filter sets so the callback
        // is invoked in maximal runs of (enabled AND changed) entities.
        template<bool UseAdded>
        ASTRA_FORCEINLINE static void ExcludeNotNewer(const EntityTicks* ticks, size_t count, Tick since, uint64_t* excluded, bool& anyExcluded) noexcept
        {
            for (size_t i = 0; i < count; ++i)
            {
                const Tick t = UseAdded ? ticks[i].added : ticks[i].changed;
                const uint64_t bit = static_cast<uint64_t>(!IsNewer(t, since));
                excluded[i >> 6] |= bit << (i & 63);
                anyExcluded |= (bit != 0);
            }
        }

        // One excluded-mask pass per change-TRACKED term of the tuple; untracked
        // terms contribute nothing here (the chunk reject already decided them).
        // An empty tuple makes the fold a no-op, hence the [[maybe_unused]]s.
        template<bool UseAdded, typename... Ts>
        ASTRA_FORCEINLINE static void BuildExcluded([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                                    [[maybe_unused]] size_t count, [[maybe_unused]] Tick since,
                                                    [[maybe_unused]] uint64_t* excluded, [[maybe_unused]] bool& anyExcluded, std::tuple<Ts...>*) noexcept
        {
            ((IsChangeTrackedV<Ts>
                ? ExcludeNotNewer<UseAdded>(chunk->GetTicks(cm.idToColumn[TypeID<std::remove_const_t<Ts>>::Value()]), count, since, excluded, anyExcluded)
                : void()), ...);
        }

        // Per-chunk filter for one chunk (shared by the serial and parallel paths):
        // the Changed/Added version reject, then the three-tier enabled-only filter.
        // count==0 chunks are no-ops. `now` is the caller's hoisted CurrentTick()
        // for the coarse change-detection stamp; Stamp=false (the internal count/
        // locate pass) instantiates no stamp at all. `since` is the change-filter
        // tick; a view without Changed/Added terms instantiates no reject code.
        template<bool Stamp, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void VisitChunkFiltered(Archetype* archetype, ArchetypeChunk* chunk, Func&& func, Tick now, Tick since,
                                                  std::index_sequence<ReqIs...> reqSeq, std::index_sequence<OptIs...> optSeq)
        {
            const size_t count = chunk->GetCount();
            if (count == 0) ASTRA_UNLIKELY
                return;

            const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();

            if constexpr (HasChangeFilter)
            {
                // Chunk reject first, always (spec §2.2): a chunk whose T column is not
                // newer than `since` for ANY Changed/Added term has nothing for us.
                // Sits BEFORE the enabled whole-chunk reject and BEFORE the coarse
                // stamp: a rejected chunk is not "visited" and must not be stamped.
                // Unhinted on purpose: in the steady state most chunks are unchanged
                // per frame, so the reject is the common outcome, not the rare one.
                if (!ChangeChunkPasses(chunk, cm, since))
                    return;
            }
            else
                (void)since;

            std::array<bool, sizeof...(OptIs)> hasOptional =
            {
                archetype->HasComponent<std::tuple_element_t<OptIs, OptionalTypes>>()...
            };
            std::tuple<std::tuple_element_t<ReqIs, RequiredTypes>*...> reqPtrs =
            {
                chunk->GetComponentArray<std::tuple_element_t<ReqIs, RequiredTypes>>()...
            };
            std::tuple<std::tuple_element_t<OptIs, OptionalTypes>*...> optPtrs =
            {
                (hasOptional[OptIs] ? chunk->GetComponentArray<std::tuple_element_t<OptIs, OptionalTypes>>() : nullptr)...
            };
            const auto& entities = chunk->GetEntities();

            constexpr size_t NReq = std::tuple_size_v<EnabledRequiredFilter>;
            const uint64_t* wordSets[NReq + 1];               // enabled sets + (optional) excluded set
            bool allZero = true;
            const bool anyReqFull = ResolveRequiredFilter(chunk, cm, count, wordSets, allZero, std::make_index_sequence<NReq>{});
            if (anyReqFull) ASTRA_UNLIKELY
                return;   // Tier 2: a required column is fully disabled -> skip whole chunk
            size_t setCount = NReq;

            // Per-entity change tier (spec §3.4 step 3): for every change-TRACKED
            // Changed<T>/Added<T> term, scan the tick column of this (version-passing)
            // chunk into an "excluded" word set (SET bit == entity fails the term) and
            // union it with the enabled sets below. Untracked terms were fully decided
            // by the chunk reject above. Deliberately AFTER the whole-chunk enabled
            // reject (an all-disabled chunk never pays the tick scan) and BEFORE the
            // Tier-1/Tier-3 split. Not a reject: the chunk is still "visited" (Ruling B).
            [[maybe_unused]] SmallVector<uint64_t, 64> excluded;   // 64 inline words = 4096 slots before a heap step
            if constexpr (HasTrackedChangeTerm)
            {
                excluded.assign((count + 63) >> 6, 0ull);
                bool anyExcluded = false;
                BuildExcluded<false>(chunk, cm, count, since, excluded.data(), anyExcluded, static_cast<ChangedTypes*>(nullptr));
                BuildExcluded<true >(chunk, cm, count, since, excluded.data(), anyExcluded, static_cast<AddedTypes*>(nullptr));
                if (anyExcluded)
                {
                    wordSets[setCount++] = excluded.data();
                    allZero = false;                       // a per-entity constraint exists: no Tier-1 whole-chunk call
                }
            }

            // Tick columns for Mut<T> / optional marks (tracked yields only); after
            // both whole-chunk rejects, like the stamp below (Ruling B).
            [[maybe_unused]] EntityTicks* reqTicks[sizeof...(ReqIs) == 0 ? 1 : sizeof...(ReqIs)] = {};
            [[maybe_unused]] EntityTicks* optTicks[sizeof...(OptIs) == 0 ? 1 : sizeof...(OptIs)] = {};
            if constexpr (HasTrackedYield)
                ResolveTrackedTicks(chunk, cm, hasOptional, reqTicks, optTicks, reqSeq, optSeq);

            // Coarse change-detection stamp, deliberately AFTER both whole-chunk
            // rejects above (change version, enabled): a chunk this view skips
            // wholesale is not "visited" and must not be stamped (it would
            // manufacture downstream false positives).
            if constexpr (Stamp)
                StampChunkForYields(chunk, cm, hasOptional, now, reqSeq, optSeq);

            if constexpr (HasOptionalFilter)
            {
                (CheckOptionalAllEnabled<OptIs>(optPtrs, chunk, cm, allZero), ...);
            }

            if (allZero)
            {
                // Tier 1: all relevant columns fully enabled -> pre-existing body, no bit tests.
                InvokeEntityCallback<Stamp>(entities, reqPtrs, optPtrs, count, func, reqSeq, optSeq, reqTicks, optTicks, now);
                return;
            }

            // Tier 3: mixed -> runs of the required-enabled intersection (AND the
            // per-entity change mask when one was appended), per-entity optional
            // nulling inside the runs.
            Detail::ForEachEnabledRun(wordSets, setCount, count,
                [&](size_t begin, size_t end)
                {
                    InvokeEntityCallbackFiltered<Stamp>(entities, reqPtrs, optPtrs, begin, end, chunk, cm, func, reqSeq, optSeq, reqTicks, optTicks, now);
                });
        }

        // Exact visible-entity count for a required-filtered view: enabled-in-all
        // required enableable columns, per chunk. Called only when HasRequiredFilter.
        size_t SizeFiltered(Archetype* archetype)
        {
            const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();
            size_t total = 0;
            for (auto& chunkPtr : archetype->GetChunks())
            {
                ArchetypeChunk* chunk = chunkPtr.get();
                const size_t count = chunk->GetCount();
                if (count == 0) ASTRA_UNLIKELY
                    continue;

                constexpr size_t NReq = std::tuple_size_v<EnabledRequiredFilter>;
                const uint64_t* reqWords[NReq == 0 ? 1 : NReq];
                bool allZero = true;
                const bool anyFull = ResolveRequiredFilter(chunk, cm, count, reqWords, allZero, std::make_index_sequence<NReq>{});
                if (anyFull) continue;                    // 0 visible in this chunk
                if (allZero) { total += count; continue; }

                // Mixed: subtract popcount of the disabled union, tail-masked to count.
                const size_t numWords = (count + 63) >> 6;
                size_t disabled = 0;
                for (size_t w = 0; w < numWords; ++w)
                {
                    uint64_t dis = 0;
                    for (size_t s = 0; s < NReq; ++s)
                        dis |= reqWords[s][w];
                    const size_t validBits = count - (w << 6);
                    if (validBits < 64)
                        dis &= (uint64_t(1) << validBits) - 1;
                    disabled += static_cast<size_t>(std::popcount(dis));
                }
                total += count - disabled;
            }
            return total;
        }

        std::vector<Archetype*> m_archetypes;
        std::shared_ptr<ArchetypeManager> m_archetypeManager;
        std::shared_ptr<IWorkScheduler> m_scheduler;  // null = sequential inline fallback

        uint32_t m_lastRefreshCounter = 0;
        uint32_t m_lastGeneration = 0;
        uint32_t m_lastRemovalCounter = 0;
    };

    // Compile-time read/write access sets of a View's type args, for the scheduler
    // (Stage 2). With<T>/Not<T>/Any/OneOf contribute nothing (match-only). Masks are
    // built at runtime from the type-lists, mirroring how SystemScheduler harvests
    // component masks today.
    template<typename V> struct ViewAccess;

    template<typename... Args>
    struct ViewAccess<View<Args...>>
    {
        using Reads  = typename Detail::AccessReads<Args...>::type;
        using Writes = typename Detail::AccessWrites<Args...>::type;

        static ComponentMask ReadMask()  { return MaskOf(static_cast<Reads*>(nullptr)); }
        static ComponentMask WriteMask() { return MaskOf(static_cast<Writes*>(nullptr)); }

    private:
        template<typename... Ts>
        static ComponentMask MaskOf(std::tuple<Ts...>*) { return MakeComponentMask<Ts...>(); }
    };
} // namespace Astra