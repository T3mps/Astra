#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Astra::Detail
{
    /**
     * Enabled-run extraction for enableable-components query filtering (spec §5).
     *
     * Given `setCount` word-sets (each a pointer to `(count + 63) / 64` 64-bit
     * words where a SET bit == DISABLED), invoke `fn(size_t begin, size_t end)`
     * for every maximal run of indices in `[0, count)` where NO set has a bit
     * set -- i.e. every maximal span of ENABLED-in-all entities. Runs are visited
     * in ascending index order and are stitched across word boundaries so a span
     * of enabled entities that straddles bit 63/64 is a single call (invariant:
     * visit order == ascending index within runs).
     *
     * `setCount == 0` yields the single run `[0, count)` (no constraint). Bits at
     * or beyond `count` in the final word are masked off, so callers may pass
     * whole-word regions without pre-clearing the tail.
     *
     * Pure + header-only so it is unit-testable through the view tests and shared
     * by both the serial and parallel filtered iteration paths. Uses the Mosaic
     * BitSet countr_zero/countr_one scan idiom generalised to ranges.
     */
    template<class Fn>
    inline void ForEachEnabledRun(const uint64_t* const* wordSets, size_t setCount,
                                  size_t count, Fn&& fn)
    {
        if (count == 0)
            return;

        const size_t numWords = (count + 63) >> 6;
        bool   open = false;    // a run left open at the top of the previous word
        size_t openStart = 0;

        for (size_t w = 0; w < numWords; ++w)
        {
            // Union the disabled words across every set, then invert to enabled.
            uint64_t disabled = 0;
            for (size_t s = 0; s < setCount; ++s)
                disabled |= wordSets[s][w];
            uint64_t e = ~disabled;

            // Mask off bits at or beyond count in the final (possibly partial) word.
            const size_t base = w << 6;
            const size_t validBits = count - base;          // in [1, 64] for the last word
            if (validBits < 64)
                e &= (uint64_t(1) << validBits) - 1;

            // Stitch a run carried over from the previous word.
            if (open)
            {
                if (e & 1ull)
                {
                    const int cont = std::countr_one(e);
                    if (cont == 64)
                        continue;                            // whole word continues the run
                    fn(openStart, base + static_cast<size_t>(cont));
                    open = false;
                    e &= ~((uint64_t(1) << cont) - 1);       // consume the leading ones
                }
                else
                {
                    fn(openStart, base);                     // run ended at the word boundary
                    open = false;
                }
            }

            // Scan the remaining maximal one-runs inside this word.
            while (e != 0ull)
            {
                const int start = std::countr_zero(e);
                const int len   = std::countr_one(e >> start);
                const int end   = start + len;               // exclusive, within (0, 64]
                if (end == 64)
                {
                    open = true;                             // reaches the top: carry to next word
                    openStart = base + static_cast<size_t>(start);
                    break;
                }
                fn(base + static_cast<size_t>(start), base + static_cast<size_t>(end));
                e &= ~(((uint64_t(1) << len) - 1) << start); // clear [start, end)
            }
        }

        if (open)
            fn(openStart, count);
    }
} // namespace Astra::Detail
