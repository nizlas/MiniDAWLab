# Architecture principles

This document defines the **timeless** architecture envelope for MiniDAWLab: separations, ownership, threading, and safety rules that should stay true across phases.

**Present-day wiring** (types, maps, file paths, current commands, schema version) belongs in **[`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md)** — read it alongside this file before implementing substantive work. Treat [`docs/PHASE_PLAN.md`](PHASE_PLAN.md) as **historical process and intent**, not the live map of the codebase.

The agent implements **within** this envelope plus the other steering documents. It must **not invent** new persistent owners, realtime contracts, transport semantics, or subsystems unless steering is updated **first**.

The product is **timeline- and engine-shaped** from the start—a small playback engine with staged features—not a disposable file player whose architecture would need a rewrite for multiple clips/tracks.

---

## Core rule

Architecture is negotiated in **documents** before it is asserted in **code**.

If gaps, ambiguities, or violations appear, pause and reconcile steering (including [`docs/IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md) escalation rules) rather than patching forward in isolation.

Growth must be **incremental**: the codebase should scale from simple cases to richer timelines without poisoning later refactors—but **without** speculative subsystems that are out of steering scope ([`docs/PHASE_PLAN.md`](PHASE_PLAN.md) lists what phases committed to).

**Hazard patterns** (stay wrong even when prototypes are minimal): UI owning transport truth; playback logic trapped in presentation components; collapsing clip identity into transport identity; import creating playback machinery with no separable domain model.

---

## Layering and coupling

These boundaries are **non-negotiable**:

- **UI** does not own engine/playback truth. It displays state and issues **high-level** actions; it does not become the hidden owner of transport, decoding flow, or mix policy.
- **File import / decode** is separate from **playback execution** and from **transport policy**. Opening a path must not blur those roles.
- **Waveform / visualisation** is separate from **audio callback** work. Rendering may read clip/material metadata; it must not drive or entangle realtime processing.
- **No hidden singletons** unless explicitly justified in steering and visible in design.
- **Ownership and lifetimes** must be legible: who owns what, what is long-lived vs view-local, what is non-owning, and where destruction order matters.

For how these map to today’s classes and threads, see **[`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md)**.

---

## Transport: source of truth and writers

Transport state has **one** conceptual owner for **live** playback control: intent (play/pause/stop), **authoritative playhead** position, and **pending seek**. These must not be duplicated across unrelated objects without an explicit, documented model.

**Timeless write rules** (exact types and call sites: [`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md)):

- **Playback intent** is written from **non-realtime** code in response to user/session actions.
- The **authoritative playhead** is advanced only from the **audio callback** path that owns playback integration (today: the engine side that applies time).
- **Seek** is requested from **non-realtime** code as a **pending** request; the audio path **consumes** it at a safe point and applies it to the playhead. The message thread must **not** silently overwrite the live playhead as if it were the callback.
- **Project load:** restoring a saved playhead is expressed only via the **approved seek pathway** once a coherent session snapshot exists for playback—**not** by inventing parallel playhead storages tied to snapshots. Live transport remains the sole owner of **live** playhead/seek-pending semantics.

Timeline meaning of the playhead (clip-relative vs timeline-absolute samples, rulers, lanes) follows the **published session model**. Do not re-derive overlapping concepts in UI-owned state.

---

## Session snapshots and placement semantics

**Immutability and publication:**

- **`SessionSnapshot` is immutable.** Edits produce a **new** snapshot value; mutations do not patch a shared instance in place.
- The audio thread observes session placement via a **released published view** (e.g. atomic `shared_ptr` to `const`): **non-blocking, lock-free** hot-path reads consistent with steering; allocation and heavyweight work stay off the realtime path unless explicitly justified elsewhere.

**What the domain owns:**

- Clip **order**, **placement** on the shared timeline/lanes, overlap resolution, track row order, lane kind distinctions, **non-destructive trim windows** versus raw PCM—all are **session/snapshot-owned** truths. UI is a **consumer** of that truth for display and gestures; it must not secretly become the canonical store for overlap order or timeline geometry.
- **Order changes** (add, move, reorder tracks, trim windows, cross-lane moves) happen only through **explicit, named** session/snapshot/document operations—not as side effects of selection, hover, or in-flight drag state.
- **Non-destructive editing:** PCM buffers are **not** shortened unless product/steering explicitly calls for destructive edit; audible span and timeline extent follow **effective placement** semantics. (Concrete rules today: [`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md).)

Instrument rows, playback bridges keyed by **`TrackId`**, and experimental/project bindings are **current fact**, not exceptions—design new work against **`docs/CURRENT_ARCHITECTURE.md`**, not against legacy “single instrument” metaphors.

---

## Audio-thread and realtime integrity

Treat the device callback as a **hard realtime environment**:

- Prefer **no** file I/O, decode setup, UI, waveform builds, heavyweight allocation, mutex/condition-variable waits, or unclear cross-thread mutation on that path unless explicitly documented and justified.
- If additional threads or queues appear, document **producer/consumer**, **blocking**, **loss**, **ordering**, and **what the callback may touch** **before** coding.

Legacy cross-thread summaries (Phase 1 message thread vs audio thread) distill to: **callbacks read published views and advance time; session and heavy I/O stay off that path** except for the approved handoffs.

---

## Instrument / plugin inserts (threading and persistence)

**Runtime vs snapshot:**

- **Live plugin instances are not part of `SessionSnapshot`.** They are mutable, may own GUI, and are created/destroyed on the **message thread** only.
- A **host/registry** on the message thread owns instances; it publishes a **separate atomic `shared_ptr` to an immutable “active processor view”** for the callback—**same handoff discipline** as the session snapshot, **different** payload and pointer.
- **`prepareToPlay` / `releaseResources` / load / editor UI** run on the message thread. The callback **reads** the active view and calls **`processBlock`** into **pre-sized** scratch buffers: **no locks, no heap on that path**, no direct `Session` mutation from audio.

**Persistence:** plugin identity and state blobs are persisted per steering (paths, identifiers, opaque state). They are **not** clip PCM; they follow plugin persistence policy, distinct from **`Audio/`** project-relative audio asset rules unless steering explicitly aligns them.

**Realtime caveat:** third-party `processBlock` is **not** certified allocation-free—that is accepted only where steering says so; do not confuse “our hot path avoids locks” with “all hosted code is realtime-safe.”

---

## Recording coordination (narrow contract)

Minimal input recording stays **transport-wired at composition**: the app root coordinates transport intents, placement, and begin/stop; a capture service **must not** quietly own **`Transport`** or transport policy.

**Audio path:** realtime-safe enqueue only (e.g. SPSC-style handoff); file writers, **`PlacedClip` / snapshot** commits, and non-realtime bookkeeping run **off** the callback **in documented order**.

Details and device/FIFO rules remain in [`docs/PHASE_PLAN.md`](PHASE_PLAN.md) / project decision logs where referenced—this file only pins **ownership** and **thread boundaries**.

---

## Scope and subsystems

Do **not** introduce major frameworks or cross-cutting subsystems (full mixing graphs, general job systems, broad plugin platforms beyond committed slices, etc.) unless they are **explicitly added to steering first**.

[`docs/PHASE_PLAN.md`](PHASE_PLAN.md) names which slices exist; **`docs/CURRENT_ARCHITECTURE.md`** says what landed in code—use both to sanity-check creep.

---

## Code expression (not policy duplication)

Prefer **modern, readable C++** when it improves correctness and clarity—not cleverness for its own sake.

Responsibility, ownership, threading, and realtime boundaries must remain **visible in central code**. The **six-tier** in-code rubric, readability-refactor allowances, exemptions, and validation gate live only in **[`docs/CODE_DOCUMENTATION_RUBRIC.md`](CODE_DOCUMENTATION_RUBRIC.md)**—apply that document **when rubric-bearing work applies** ([`docs/IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md) summarizes day-to-day vs full pass).

---

## Questions to answer before a slice

Explicitly decide (and document in planning output if non-trivial):

1. Who owns **transport** intent, playhead, and seek pend for this slice?
2. Who owns **clip/material** lifecycle and timeline placement truths?
3. What runs on **audio thread** versus **message thread**, and where is snapshot/publication boundary?
4. What is assumed about **minimal** timelines or single-lane prototypes that must **not** leak into coupling?
5. What is deliberately **deferred**, and why is that safe?

If these cannot be answered without guessing, update steering before coding.
