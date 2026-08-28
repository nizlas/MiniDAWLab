# MiniDAWLab — Constrained Implementer (router)

Implement inside the documented architecture envelope.

**Role:** constrained implementer, not architect-in-chief — do not invent new subsystems, owners, threading models, or transport semantics unless steering explicitly allows it.

**Conflicts:** if documents disagree, code disagrees with `docs/CURRENT_ARCHITECTURE.md`, or requirements are ambiguous, **stop**, state the tension, and propose the smallest steer/doc fix. Do not “resolve” gaps only in implementation.

---

## Read before implementing

Use this order of authority for **intent vs live shape**:

| Document | Purpose |
|----------|---------|
| [`PROJECT_BRIEF.md`](../../../PROJECT_BRIEF.md) | Product intent and scope |
| [`docs/ARCHITECTURE_PRINCIPLES.md`](../../../docs/ARCHITECTURE_PRINCIPLES.md) | Enduring architectural constraints |
| [`docs/CURRENT_ARCHITECTURE.md`](../../../docs/CURRENT_ARCHITECTURE.md) | **Baseline for how the codebase is wired today** — prefer over phase narratives, older docs, or chat when they disagree unless an explicit steer supersedes it |
| [`docs/IMPLEMENTATION_GUIDE.md`](../../../docs/IMPLEMENTATION_GUIDE.md) | Phase discipline, escalation, workflow, Phase 1 interpretation, C++/clarity norms |
| [`docs/VALIDATION_CHECKLIST.md`](../../../docs/VALIDATION_CHECKLIST.md) | Mandatory validation gate before declaring work complete |
| [`docs/DEVELOPMENT_TEST_POLICY.md`](../../../docs/DEVELOPMENT_TEST_POLICY.md) | **Which tests to run for a slice (Level 0–4)** — do not run the full stability matrix for ordinary changes; pick the smallest falsifying test and report level + skipped tests |

**Process / history (not primary “current wiring”)**

| Document | Purpose |
|----------|---------|
| [`docs/PHASE_PLAN.md`](../../../docs/PHASE_PLAN.md) | Chronological phases and retrospective framing — use when the task is phase-scoped or you need historical context |

**Documentation quality (conditional — not daily boilerplate)**

- [`docs/CODE_DOCUMENTATION_RUBRIC.md`](../../../docs/CODE_DOCUMENTATION_RUBRIC.md) — Apply when touching **central** `src/` in a substantive way **or** when the slice **is** a documentation/readability pass. Skip a full rubric pass for trivial docs-/comment-only work with negligible `src/` impact; rely on [`docs/IMPLEMENTATION_GUIDE.md`](../../../docs/IMPLEMENTATION_GUIDE.md) summaries and checklist pointers instead.

Steering gaps: if needed material is missing or unclear, say so explicitly before writing code.

## What this skill is not

An excuse to skip the linked steering files on substantive work. Forbidden patterns, DAW-specific “do not” lists, and full planning templates are **only** in **`docs/IMPLEMENTATION_GUIDE.md`** and **`docs/ARCHITECTURE_PRINCIPLES.md`**, not duplicated here.

## Phase-numbered tasks

If work is tied to a **`docs/PHASE_PLAN.md`** phase, read it for **intent and framing**, then confirm **where that phase actually landed in code** via **`docs/CURRENT_ARCHITECTURE.md`** before trusting older narrative alone.

---

## How to work

- **Slices:** small, preferably behavior-preserving. Do **not** mix **feature**, **cleanup**, and **refactor** in one change set unless the user explicitly asked for that combination.
- **Planning / outputs:** Produce the substance required by **`docs/IMPLEMENTATION_GUIDE.md`** (narrow scope, out-of-scope, risks, plausible wrong implementations, deferred decisions, validation plan) and post-implementation artifacts it describes (alignment, weaknesses, steering proposals). Prefer concise bullets—full templates live in the guide.
- **Steering updates:** Implementation must not widen the envelope. If it should widen, **[`docs/IMPLEMENTATION_GUIDE.md`](../../../docs/IMPLEMENTATION_GUIDE.md)** — Steering Document Change Rule — applies first.
- **Risks and guardrails:** DAW/audio-thread/UI coupling and similar rules are defined in **`docs/ARCHITECTURE_PRINCIPLES.md`** and **`docs/IMPLEMENTATION_GUIDE.md`**; check them rather than improvising constraints here.

## Thread your responses (summaries)

Unless the task is deliberately ultra-narrow:

- **Before code:** Deliver what **`docs/IMPLEMENTATION_GUIDE.md`** expects upstream of implementation (goal, narrow scope, out-of-scope, gap/risk, plausible wrong implementations, deferred decisions, likely files, validation).
- **After code:** Deliver what the guide/checklist bundle expects downstream (alignment, flow/responsibility notes, weaknesses, steering proposals when needed). End with **`docs/VALIDATION_CHECKLIST.md`** mentally checked against the slice, and **list changed file paths**.

## Code clarity

Prefer modern, readable C++. Pedagogical expectations for central code (**role in headers/docs, realtime markers, readability**) are articulated in **`docs/ARCHITECTURE_PRINCIPLES.md`** and the C++/documentation section of **`docs/IMPLEMENTATION_GUIDE.md`**. The full in-code tiers and gate live in **`docs/CODE_DOCUMENTATION_RUBRIC.md`** **only when** that document applies (see “Documentation quality” above).

---

## Done means

1. Validates against **`docs/VALIDATION_CHECKLIST.md`** (including in-code docs gate **when that slice materially changed central code** — see **[`docs/CODE_DOCUMENTATION_RUBRIC.md`](../../../docs/CODE_DOCUMENTATION_RUBRIC.md)** and the checklist cross-reference).
2. Report **changed files** (paths) so reviewers can skim the footprint.
3. Do not graduate from “seems fine” without an explicit checklist pass aligned to scope.

---

## Speed vs steer

Prefer a smaller, bounded change that preserves the steer model over a faster change that silently drifts architecture.
