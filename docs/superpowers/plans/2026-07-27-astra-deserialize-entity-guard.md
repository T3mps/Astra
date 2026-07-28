# ArchetypeManager::Deserialize Entity-Id Guard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the verified untrusted-load P0: `ArchetypeManager::Deserialize` must reject (not allocate for) crafted/corrupt mapping entity ids, via a non-creating record lookup + aliveness cross-check.

**Architecture:** One call-site change in `ArchetypeManager::Deserialize`'s mapping loop (`GetOrCreateRecord` → `GetRecord` + null/version predicate, `return false` on mismatch) plus corruption tests in the existing load-robustness suite. Full rationale + exact code in the spec.

**Tech Stack:** C++20 header-only; MSBuild; GoogleTest.

**Spec:** `docs/superpowers/specs/2026-07-27-astra-deserialize-entity-guard-design.md` (contains the exact replacement code block and the verified line references — it is the source of truth).

## Global Constraints

- Files changed: ONLY `include/Astra/Archetype/ArchetypeManager.hpp` (the one call site at ~:1030-1033 + its comment) and `tests/Serialization/LoadRobustnessTest.cpp`. Nothing else.
- TDD required: corruption tests written first and observed RED (Load succeeds on corrupt input today), then the fix turns them GREEN with zero regressions.
- Reuse existing test component types (128-TypeID ceiling — no new component types in AstraTest).
- Follow `LoadRobustnessTest.cpp`'s existing save→corrupt-bytes→load conventions (read the file first; mirror its buffer-patching helpers and error-assertion style).
- Preserve graceful-failure semantics: the mapping loop's `return false` feeds the existing typed error path; no asserts/aborts on corrupt input (uniform-graceful policy).
- Build/test recipe (MSBuild, NOT make): `premake5 vs2022` NOT needed (no new files); build whole solution `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Cfg> -p:Platform=x64 -m` (no `-t:`; judge only by MSBuild); run `bin/<Cfg>-windows-x86_64/AstraTest/AstraTest.exe`. 3-config green gate (full suite Debug; Serialization/LoadRobustness filters Release/Dist).
- Branch: `fix/deserialize-entity-guard`.

---

### Task 1: Guarded lookup + corruption tests

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (~:1030-1033)
- Test: `tests/Serialization/LoadRobustnessTest.cpp`

**Interfaces:**
- Consumes: `EntityTable::GetRecord(IDType) -> EntityRecord*` (non-creating, nullptr when the id's segment doesn't exist — EntityTable.hpp:163-169, bounds-checked GetSegment :560-585); `EntityRecord::version` (0 = dead — EntityRecord.hpp:30); `Entity::GetVersion()`.
- Produces: no API change — hardened `Deserialize` behavior only.

- [ ] **Step 1: Read the spec and the robustness suite** — `docs/superpowers/specs/2026-07-27-astra-deserialize-entity-guard-design.md` (the exact replacement block is in §2), then `tests/Serialization/LoadRobustnessTest.cpp` end-to-end to learn its save-buffer-corruption helpers and assertion style.

- [ ] **Step 2: Write the failing tests (RED)** — per spec §3: (1) huge-id mapping corruption → Load must fail; (2) dead/never-restored-id mapping corruption → Load must fail. Use the suite's existing corruption approach (locate/patch the mapping record's entity id bytes in the saved buffer; if byte-locating proves brittle, an equivalent deterministic corruption of that field via the suite's established patterns is fine — the assertion is what matters: Load returns the error path, process does not crash). Build Debug, run the new tests, confirm BOTH fail because Load currently succeeds. Record exact RED output.

- [ ] **Step 3: Apply the fix (GREEN)** — replace the `GetOrCreateRecord` call site with the spec §2 code block verbatim (comment included). Build Debug; new tests pass; run the FULL Debug suite — zero regressions (round-trip Save→Load tests must stay green; the predicate must not reject legitimate saves).

- [ ] **Step 4: 3-config gate** — build Release and Dist; run `--gtest_filter=*Robustness*:*Serialization*` in each. Green.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Archetype/ArchetypeManager.hpp tests/Serialization/LoadRobustnessTest.cpp
git commit -m "fix(serialization): reject corrupt mapping entity ids in ArchetypeManager::Deserialize (2026-07-27 review P0)"
```

---

## Self-Review (done during planning)

Spec coverage: §2 fix → Step 3; §3 tests 1-2 → Step 2, test 3 → Step 3's full-suite gate; §4 constraints → Global Constraints. No placeholders — the exact fix code lives in spec §2 (single source of truth, referenced not duplicated). Types verified against current headers during design (GetRecord/GetSegment/version semantics all read directly this session).
