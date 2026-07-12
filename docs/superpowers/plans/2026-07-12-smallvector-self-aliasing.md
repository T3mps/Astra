# SmallVector Self-Aliasing Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `SmallVector`'s `emplace_back`/`emplace` (and the `push_back`/single-value `insert` that delegate to them) tolerate an argument that aliases an existing element — `v.push_back(v[i])` — matching `std::vector`, without regressing the hot path.

**Architecture:** Materialize a temporary from the argument *before* any reallocation or element shift on the two hazard paths only; the common at-end-with-spare-capacity insertion is left unchanged. Self-insertion becomes silently valid (no assert, no exceptions), exactly as `std::vector`.

**Tech Stack:** C++20 header-only; GoogleTest; premake5 → MSBuild (VS2022 solution). Spec: `docs/superpowers/specs/2026-07-12-smallvector-self-aliasing-design.md`. Review of record: `docs/reviews/2026-07-11-astra-full-review.md` (must-fix item 7).

## Global Constraints

- C++20; header-only; **no exceptions** (test project builds with `-fno-exceptions` — never add `throw`/`try`); no new dependencies.
- Must compile on MSVC 2022+, GCC 11+, Clang 13+ (avoid MSVC-only constructs).
- **No assert on self-insertion** — a self-referencing argument is valid input, not a bug to diagnose. Match `std::vector` semantics silently.
- Do NOT touch the common hot path: at-end insertion with spare capacity (`emplace_back` when `m_size < capacity()`) must be byte-for-byte unchanged.
- Do NOT modify `insert(pos, count, value)` (already guarded at `SmallVector.hpp:344`), `assign`, `resize`, `erase`, or `Grow`.
- Build (whole solution; `-t:AstraTest` does NOT work — projects nest in solution folders):
  `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release> -p:Platform=x64 -m -v:minimal`
- Test: `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe --gtest_brief=1`. **Gate: full suite green in Debug AND Release.** Release is where the bug lives — Release coverage is the point.
- Appending tests to the EXISTING `tests/Container/SmallVectorTest.cpp` needs NO premake regen.
- New tests MUST use `TEST_F(SmallVectorTest, ...)` — the file's suite is a fixture; a bare `TEST` on that suite hard-fails gtest at startup.
- **Commit tracked source only.** `ide/`, `Astra.sln`, `Makefile`, `*.make` are gitignored — stage only `include/` and `tests/`.
- End every commit message body with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01XF1hB4cickyZqBjL1acq6g`
- The IDE clang linter emits false positives ("expects Clang 20", "gtest not found", "no std::byte") — ignore; judge by the MSVC build.

## File structure

- Modify: `include/Astra/Container/SmallVector.hpp` — `emplace_back` (`:439-448`), `emplace` (`:372-402`).
- Modify (append tests): `tests/Container/SmallVectorTest.cpp`.

Single task: the fix and its regression tests are one cohesive, independently-reviewable deliverable.

---

### Task 1: SmallVector self-aliasing safety

**Files:**
- Modify: `include/Astra/Container/SmallVector.hpp` (`emplace_back` `:439-448`, `emplace` `:372-402`)
- Test: `tests/Container/SmallVectorTest.cpp` (append 4 `TEST_F` cases + a helper type)

**Interfaces:**
- Consumes: existing `SmallVector<T, N>` public API — `push_back`, `emplace_back`, `emplace`, `insert`, `operator[]`, `size()`, `capacity()`, `begin()`. Confirm the second template parameter `N` is the inline capacity by reading the existing tests (e.g. `SmallBufferOptimization`).
- Produces: `emplace_back`/`emplace` (and by delegation `push_back`/single-value `insert`) are correct when the argument aliases an element of the same vector. No signature changes.

- [ ] **Step 1: Add the move-observable test helper**

Append to `tests/Container/SmallVectorTest.cpp` (after the existing includes/tests; an anonymous namespace at file scope is fine). This type makes an aliased/stale read *deterministically* detectable (a moved-from source is marked `kMovedFrom`) and counts lifetimes to catch leaks/double-frees:

```cpp
namespace {
// Move-observable, lifetime-counted element for self-aliasing tests.
struct AliasProbe
{
    static inline int liveCount = 0;
    static constexpr int kMovedFrom = -1;
    int value = 0;

    AliasProbe() { ++liveCount; }
    explicit AliasProbe(int v) : value(v) { ++liveCount; }
    AliasProbe(const AliasProbe& o) : value(o.value) { ++liveCount; }
    AliasProbe(AliasProbe&& o) noexcept : value(o.value) { o.value = kMovedFrom; ++liveCount; }
    AliasProbe& operator=(const AliasProbe& o) { value = o.value; return *this; }
    AliasProbe& operator=(AliasProbe&& o) noexcept { value = o.value; o.value = kMovedFrom; return *this; }
    ~AliasProbe() { --liveCount; }
};
} // namespace
```

- [ ] **Step 2: Write the failing tests**

Append these four `TEST_F(SmallVectorTest, ...)` cases. Adapt `Astra::SmallVector<AliasProbe, N>` to the real template-parameter order/name if it differs (confirm from the existing tests); the inline-capacity values below (`4`, `2`, `8`) just need to force the described growth.

```cpp
// SBO -> heap reallocation: v.push_back(v[0]) must copy the original value,
// not read the moved-from inline slot. (Deterministic RED pre-fix: kMovedFrom.)
TEST_F(SmallVectorTest, SelfAliasingPushBackForcesReallocation)
{
    Astra::SmallVector<AliasProbe, 4> v;
    for (int i = 0; i < 4; ++i)
        v.push_back(AliasProbe(i));           // inline full: size == capacity == 4
    ASSERT_EQ(v.size(), 4u);
    ASSERT_EQ(v.capacity(), 4u);

    v.push_back(v[0]);                        // self-alias, forces reallocation
    EXPECT_GT(v.capacity(), 4u);              // grew off inline storage
    ASSERT_EQ(v.size(), 5u);
    EXPECT_EQ(v.back().value, 0);             // original value, not kMovedFrom
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(v[i].value, i);             // pre-existing elements intact
}

// Already-heap buffer: reallocation frees the storage the argument points into.
TEST_F(SmallVectorTest, SelfAliasingEmplaceBackHeapRealloc)
{
    Astra::SmallVector<AliasProbe, 2> v;
    for (int i = 0; i < 3; ++i)
        v.emplace_back(i);                    // 3rd add forces SBO -> heap
    while (v.size() < v.capacity())
        v.emplace_back(static_cast<int>(v.size()));  // fill exactly to capacity
    ASSERT_EQ(v.size(), v.capacity());

    v.emplace_back(v[0]);                     // self-alias, heap realloc frees old buffer
    EXPECT_EQ(v.back().value, 0);             // must be 0, not freed-memory garbage
}

// Middle insert shifts elements before the argument is read.
TEST_F(SmallVectorTest, SelfAliasingMiddleInsert)
{
    Astra::SmallVector<AliasProbe, 8> v;
    for (int i = 0; i < 6; ++i)
        v.push_back(AliasProbe(i));           // 0..5, spare capacity (no realloc)

    v.insert(v.begin() + 2, v[5]);            // insert a copy of v[5] at index 2
    ASSERT_EQ(v.size(), 7u);
    EXPECT_EQ(v[2].value, 5);                 // inserted value correct
    EXPECT_EQ(v[0].value, 0);
    EXPECT_EQ(v[1].value, 1);
    EXPECT_EQ(v[3].value, 2);                 // original v[2] shifted right
    EXPECT_EQ(v[6].value, 5);                 // original v[5] shifted right
}

// No leak / no double-free across self-aliasing reallocation + middle insert.
TEST_F(SmallVectorTest, SelfAliasingNoLeakOrDoubleFree)
{
    AliasProbe::liveCount = 0;
    {
        Astra::SmallVector<AliasProbe, 4> v;
        for (int i = 0; i < 4; ++i)
            v.push_back(AliasProbe(i));
        v.push_back(v[0]);                    // self-alias + realloc
        v.emplace(v.begin() + 1, v[3]);       // self-alias middle insert
        EXPECT_GT(AliasProbe::liveCount, 0);
    }
    EXPECT_EQ(AliasProbe::liveCount, 0);      // every construction matched by a destruction
}
```

- [ ] **Step 3: Build Release and confirm the tests FAIL (RED)**

Run:
`"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Astra.sln -p:Configuration=Release -p:Platform=x64 -m -v:minimal`
then `bin/Release-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SmallVectorTest.SelfAliasing*`

Expected: at least `SelfAliasingPushBackForcesReallocation` FAILS (`v.back().value` is `-1`/garbage, not `0`); the middle-insert and no-leak cases may also fail. Record the observed pre-fix output in the task report. (If a case happens to pass pre-fix by luck, note it — the deterministic RED is the SBO push_back case via `kMovedFrom`.)

- [ ] **Step 4: Fix `emplace_back`** (`include/Astra/Container/SmallVector.hpp:439-448`)

Replace the body with (guard the realloc branch only; leave the has-capacity branch unchanged):

```cpp
template<typename... Args>
reference emplace_back(Args&&... args)
{
    if (m_size == capacity())
    {
        // Grow() frees the current buffer; materialize the (possibly
        // self-aliasing) argument before that storage disappears, so
        // v.push_back(v[i]) is well-defined as std::vector guarantees.
        T tmp(std::forward<Args>(args)...);
        Grow(m_size + 1);
        std::construct_at(end(), std::move(tmp));
    }
    else
    {
        std::construct_at(end(), std::forward<Args>(args)...);
    }
    ++m_size;
    return back();
}
```

- [ ] **Step 5: Fix `emplace`** (`include/Astra/Container/SmallVector.hpp:372-402`)

Replace the body with (materialize `tmp` up front — covers both the realloc and the middle-shift hazard; the element-shift logic is otherwise identical to the original):

```cpp
template<typename... Args>
iterator emplace(const_iterator pos, Args&&... args)
{
    size_type offset = pos - cbegin();

    // Materialize before any Grow/shift: the argument may alias an element
    // that reallocation frees or that the shift below moves. std::vector
    // makes v.emplace(pos, v[j]) well-defined.
    T tmp(std::forward<Args>(args)...);

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

- [ ] **Step 6: Build Debug + Release and confirm the tests PASS (GREEN)**

Run for BOTH configs:
`...MSBuild... -p:Configuration=Debug ...` then `...Release...`
then the filter in each: `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SmallVectorTest.SelfAliasing*`
Expected: all 4 pass in both configs.

- [ ] **Step 7: Run the full suite in Debug AND Release**

`bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_brief=1` and the Release path.
Expected: full suite green both configs (previous baseline was 547 on `dev`; expect 547 + 4 = 551 here — gate on "both green + the 4 new tests present," not an absolute number). A LONE Release failure of ONLY `CompressionTest.PerformanceBenchmark` (10 MB/s threshold under CPU load) is the known flake — rerun it isolated to confirm; not a regression.

- [ ] **Step 8: Call-site audit**

Grep the codebase for self-aliasing usages and record findings in the task report (this determines whether item 7 was *live* or *latent*):
`grep -rnE "\.(push_back|emplace_back|emplace|insert)\(" include/ tests/` — scan for any call whose argument is an element of the SAME `SmallVector` (e.g. `x.push_back(x[i])`, `x.emplace_back(x.back())`). No call-site edits are expected (the fix makes them correct); document any found. State "live" if a real site exists, "latent" if none.

- [ ] **Step 9: Commit**

```bash
git add include/Astra/Container/SmallVector.hpp tests/Container/SmallVectorTest.cpp
git commit -m "fix(container): SmallVector emplace/emplace_back tolerate self-aliasing args (std::vector parity)"
```
(Include the two required trailer lines in the commit body.)

---

## After the task

Full whole-branch review (this is a single-task branch, so the task review largely is the whole-branch review) via superpowers:requesting-code-review, then superpowers:finishing-a-development-branch — merge target is `dev`. The release/version decision (whether this rides into a 3.4.2) is separate and out of this plan's scope.
