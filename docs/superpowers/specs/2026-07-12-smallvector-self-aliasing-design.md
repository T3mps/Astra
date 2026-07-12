# SmallVector Self-Aliasing Safety — Design Spec

**Date:** 2026-07-12
**Status:** Approved (design)
**Origin:** Must-fix item 7 of the 2026-07-11 full-codebase review (`docs/reviews/2026-07-11-astra-full-review.md`, §3) — the one Phase-1 (release-safety) item not covered by the 3.4.1 remediation.

## Problem

`SmallVector`'s single-element growth mutators consume their argument *after* an operation that can invalidate it, so an argument that aliases an existing element is read from freed or shifted-away storage. This violates the `std::vector` contract, which guarantees `v.push_back(v[i])` / `v.emplace(pos, v[j])` work correctly.

Two distinct hazards, both in `include/Astra/Container/SmallVector.hpp`:

1. **Reallocation hazard** — `emplace_back` (`:439-448`) does `Grow(m_size + 1)` (which frees the old buffer) *before* `std::construct_at(end(), std::forward<Args>(args)...)`. If `args` references an element of the old buffer, it dangles after `Grow`. `push_back(const T&)` / `push_back(T&&)` (`:429-437`) inherit this via `emplace_back`.
2. **Middle-shift hazard** — `emplace(pos, args...)` (`:372-402`), even without reallocation, does `construct_at(end(), move(back()))` → `move_backward(it, end()-1, end())` → `construct_at(it, args...)`. The shift moves/overwrites elements in `[it, end())` *before* `args` is read, so an aliasing `args` reads a moved-from or displaced value. `insert(pos, const T&)` / `insert(pos, T&&)` (`:326-334`) inherit this via `emplace`.

`insert(pos, count, value)` (`:336-370`) is **already correct** — it copies `T valueCopy(value)` (`:344`) before growing/shifting (fixed in the 3.4 hygiene sweep, commit `b441b3a`). It is left unchanged.

The review classifies this as a *latent contract gap* ("`std::vector` guarantees this works... calibrate against real call sites"): reachable in ordinary Release use, but whether Astra's own code triggers it today depends on call sites.

## Approach

**Materialize a temporary on the hazard paths only** (chosen over `std::vector`'s construct-into-new-storage strategy for simplicity and consistency with the existing `insert(count)` guard). The common hot path — at-end insertion with spare capacity, i.e. the typical `push_back`/`emplace_back` — is left byte-for-byte unchanged and pays nothing.

**Invariant:** self-insertion becomes *silently valid*, exactly as `std::vector`. No assert is added (a self-referencing argument is legal input, not a bug to diagnose). No exceptions (`-fno-exceptions`).

## Changes — `include/Astra/Container/SmallVector.hpp`

### `emplace_back(Args&&... args)` (`:439-448`)
Guard the realloc branch; leave the has-capacity branch unchanged:

```cpp
template<typename... Args>
reference emplace_back(Args&&... args)
{
    if (m_size == capacity())
    {
        // Grow() frees the current buffer; materialize the (possibly
        // self-aliasing) argument before that storage disappears.
        T tmp(std::forward<Args>(args)...);
        Grow(m_size + 1);
        std::construct_at(end(), std::move(tmp));
    }
    else
    {
        std::construct_at(end(), std::forward<Args>(args)...); // hot path, unchanged
    }
    ++m_size;
    return back();
}
```

Cost: one extra move-construct **only** on the amortized-rare reallocation. `T` is already required to be move-constructible (Grow relocates elements), so this compiles for every valid `SmallVector<T>`.

### `emplace(pos, Args&&... args)` (`:372-402`)
Materialize `T tmp` up front — this covers both the realloc and the middle-shift hazard. `emplace(pos)` is not the hot path (bulk insertion uses `emplace_back`/`insert(count)`), so always-materialize is acceptable and keeps the logic simple:

```cpp
template<typename... Args>
iterator emplace(const_iterator pos, Args&&... args)
{
    size_type offset = pos - cbegin();
    T tmp(std::forward<Args>(args)...);   // materialize before any Grow/shift

    if (m_size == capacity())
        Grow(m_size + 1);

    iterator it = begin() + offset;
    if (it == end())
    {
        std::construct_at(end(), std::move(tmp));
    }
    else
    {
        std::construct_at(end(), std::move(back()));
        std::move_backward(it, end() - 1, end());
        std::destroy_at(it);
        std::construct_at(it, std::move(tmp));
    }
    ++m_size;
    return it;
}
```

### Unchanged
- `push_back(const T&)` / `push_back(T&&)` — delegate to the now-safe `emplace_back`.
- `insert(pos, const T&)` / `insert(pos, T&&)` — delegate to the now-safe `emplace`.
- `insert(pos, count, value)` — already guarded.
- `erase`, `assign`, `resize`, `reserve`, `Grow` — out of scope; no aliasing hazard from a caller-supplied element reference.

## Call-site audit

Grep the codebase (`include/`, `tests/`) for self-aliasing usages — `container.push_back(container[...])`, `emplace_back` / `emplace` / single-value `insert` whose argument is an element of the same `SmallVector`. Record in the task report whether item 7 is currently **live** (a real call site triggers it) or purely **latent**. No call-site edits are expected — the container fix makes any such site correct — but any found are documented.

## Tests — Release-mode negative tests

Append to the existing SmallVector test file under `tests/Container/`. Each must fail (read garbage / corrupt) before the fix and pass after (demonstrate RED→GREEN), and must be meaningful in **Release** (where the pre-fix UB is unguarded):

1. **`push_back` self-alias forcing reallocation** — `reserve` exactly to capacity, then `push_back(v[0])`; assert the new last element equals the original `v[0]` value. Cover both the SBO (small, inline storage) and heap-grown states.
2. **`emplace_back` self-alias forcing reallocation** — same via `emplace_back` with an argument referencing an element.
3. **Middle `insert`/`emplace` self-alias** — `insert(begin() + k, v[j])` with `j` in the shifted range `[k, size)`; assert the correct value lands at `k` and no element is corrupted.
4. **Destructor accounting** — a `T` that counts constructions/destructions; after a self-aliasing realloc, assert no leak and no double-free (constructions == destructions at teardown). Directly addresses the review's "no destructor/leak accounting" gap for this path.

## Non-goals (YAGNI)

- No broader `std::vector`-contract audit of `SmallVector` (exception safety, `assign`/`resize` edge cases, iterator-invalidation documentation) — that is a separate, larger effort.
- No self-insertion assert or Debug diagnostic (self-insertion is valid).
- No changes to `assign`, `resize`, `erase`, or `Grow`.
- No version bump in this spec's scope (release/version decision is separate).

## Gate

- Build the whole solution and run the full `AstraTest` suite green in **Debug AND Release** (the bug lives in Release; Release coverage is the point). Dist optional.
- The new tests present and passing; existing tests unaffected.
- Commit tracked source only (`include/`, `tests/`); `ide/`, `Astra.sln`, `Makefile`, `*.make` are gitignored.
