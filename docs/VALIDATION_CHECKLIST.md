# Validation checklist

Use this after **meaningful** implementation slices—not as a full regression matrix. If a gate does not apply, say **N/A** and why.

**Live wiring and invariants** are in **[`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md)**. **Phase-by-phase acceptance detail** lives in **[`docs/PHASE_PLAN.md`](PHASE_PLAN.md)** and related logs (e.g. [`status/DECISION_LOG.md`](../status/DECISION_LOG.md)); this file does not duplicate those checklists.

---

## 1. Always

- **Build / compile:** Project builds (or document why build is out of scope for a pure-doc slice).
- **Changed files:** List paths touched (or “none”) so reviewers can see footprint.
- **Steering alignment (quick):** Slice still matches [`PROJECT_BRIEF.md`](../PROJECT_BRIEF.md), [`docs/ARCHITECTURE_PRINCIPLES.md`](ARCHITECTURE_PRINCIPLES.md), and [`docs/IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md)—or call out intentional proposals pending approval.
- **Architecture truth:** No claim or change that **contradicts** [`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) unless steering was explicitly updated in the same thread of work.
- **Behavior-preserving slices:** State what behavior was held constant and how you checked (build, targeted test, or manual step).

---

## 2. Conditional gates (turn on when the slice touches the area)

| If you touched… | Then at least… |
|-----------------|----------------|
| **Audio callback, engine hot path, transport handoff, atomics, or thread docs** | Re-read realtime rules in [`docs/ARCHITECTURE_PRINCIPLES.md`](ARCHITECTURE_PRINCIPLES.md) + callback boundaries in [`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md). Confirm no new lock/alloc/block/`Session` mutation on the realtime path unless explicitly steered. |
| **Project file, `Audio/`, paths, migration, or load/save UX** | Manual **save → reload** (and version round-trip if schema changed). Confirm errors don’t trash the current session; align with project sections in [`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md). |
| **`Session`, `SessionSnapshot`, history, or commands that mutate session** | **Undo/redo** for the edited surface (at least one round-trip on the affected action). |
| **UI components, layout, or interaction** | Short **manual smoke**: primary user flow for the control you changed; no obvious layout/gesture regressions. |
| **Instrument host, controller maps, playback bridge, VST3 load/rescan, or per-track plugin state** | Verify **per-`TrackId`** behavior matches [`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) (no accidental single-instrument assumptions). Plugin path: message-thread lifecycle vs audio-thread **read-only processor view** per principles. Rescan: parent must not corrupt loaded instance on failure (see current doc). |
| **Recording / input FIFO / take files** | Use **Phase 4** expectations in [`docs/PHASE_PLAN.md`](PHASE_PLAN.md) + decision logs—callback push path must stay realtime-safe; finalize order and snapshot publish order must match steering. |
| **Central `src/` (substantive edit or new file)** | Pass the in-code documentation **hard gate** in **[`docs/CODE_DOCUMENTATION_RUBRIC.md`](CODE_DOCUMENTATION_RUBRIC.md)** (tiers, exemptions, documentation-pass output when applicable). Trivial comment-only / negligible central delta: follow [`docs/IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md) summary; full rubric optional per rubric **Exemptions**. |

---

## 3. Core questions (short)

Answer in the completion note when non-trivial:

1. Scope stayed within agreed slice? Any hidden architecture?
2. Ownership and thread story still clear?
3. What is weak, deferred, or risky?
4. Any steering updates needed before the next slice?

---

## 4. Plausible-wrong-implementations (one pass)

Call out if relevant: “looks fine in the UI” but violates transport ownership, snapshot publication, UI-as-truth, or realtime boundaries—see risk themes in [`docs/ARCHITECTURE_PRINCIPLES.md`](ARCHITECTURE_PRINCIPLES.md).

---

## 5. Report template (e.g. Composer / final message)

Keep it scannable:

1. **Alignment** — brief yes/no + caveats  
2. **Validation run** — build; which conditional rows above were exercised  
3. **Changed files** — list  
4. **Risks / follow-ups** — bullets  

---

## 6. Done / not done

**Not done** if: violates steering, hides architectural choices, breaks agreed behavior without documenting it, or skips an applicable row in **§2** without justification.

**Done** when: functional for the slice, checklist satisfied (with N/A explained), weaknesses stated, and **in-code rubric** satisfied for every **meaningfully changed central file** when **§2** requires it.

---

## 7. Historical phase checklists

Detailed **Phase 2–4, 8**, ruler/trim/reorder, etc. acceptance lists are **not** maintained here. When validating a change that maps to a finished phase, use **[`docs/PHASE_PLAN.md`](PHASE_PLAN.md)** and **[`status/DECISION_LOG.md`](../status/DECISION_LOG.md)** as the authoritative historical acceptance references.
