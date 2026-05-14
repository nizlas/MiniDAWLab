# Phase plan (current stub)

This document is **only** for **current and near‑term slice planning**.

- It is **not** the source of truth for **how the code is wired today**. For that, read **[`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md)** first.
- The **Detailed historical phased goals, acceptance nuances, and long steering text** moved to **[`docs/ARCHIVE/PHASE_PLAN_FULL.md`](ARCHIVE/PHASE_PLAN_FULL.md)** (verbatim archive). Quote or open that file when you need exact phase wording.

Use [`docs/IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md), [`docs/VALIDATION_CHECKLIST.md`](VALIDATION_CHECKLIST.md), and [`docs/ARCHITECTURE_PRINCIPLES.md`](ARCHITECTURE_PRINCIPLES.md) for workflow, gates, and timeless constraints—this stub does not duplicate them.

---

## Current focus (now)

**Steering / doc slimming and architecture–workflow stabilization** before growing new product surface. Prefer small, reviewable slices with an explicit checklist pass.

---

## Near-term stabilization backlog (ordered-ish)

Suggested next candidates (each its **own slice** unless the user bundles explicitly):

| Item | Notes |
|------|--------|
| **Diagnostic log gating** | Keep always-on logs from spamming normal users; compile‑time / config gates where appropriate ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — diagnostics). |
| **Stale singleton‑era comments** | Align headers/comments with **`TrackId`‑keyed**, multi‑instrument reality; [`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md). |
| **Dead helper cleanup** | Remove unused helpers naming “primary” / single‑slot-era APIs where safe ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — naming debt). |
| **Later: `InstrumentRuntimeRegistry` extraction** | Pull instrument registry / wiring clutter out of **`Main.cpp`** when ready—**preserve behavior** ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — composition pointers). |

---

## Explicitly deferred (unless the user resumes them)

- **HALion** and broad third‑party instrument policy — out of scope until steered ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — non‑goals).
- **Richer plugin picker / scanner UX** beyond current steering — defer unless phased.
- **Rescan crash hardening** (child‑process failures) — **known risk**, fix **only** when scheduled ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — Rescan).

---

## Slice discipline (reminder)

**Do not** mix **feature work**, **cleanup**, and **structural refactor** in one slice unless the user explicitly requests that combo. Keep changes **narrow** and **behavior‑preserving** when the slice is labeled cleanup/refactor.

---

## Links

| Question | Doc |
|---------|-----|
| What does the codebase do today? | [`CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) |
| Full phase history text | [`ARCHIVE/PHASE_PLAN_FULL.md`](ARCHIVE/PHASE_PLAN_FULL.md) |
| How to implement / escalate | [`IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md) |
| How to validate | [`VALIDATION_CHECKLIST.md`](VALIDATION_CHECKLIST.md) |
