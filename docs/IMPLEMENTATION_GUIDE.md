# Implementation Guide

This document defines how implementation work must be carried out in MiniDAWLab.

The agent is a constrained implementer, not architect-in-chief.  
It may only implement within the documented architecture envelope.

If gaps, ambiguities, hidden assumptions, or architectural risks are found, the correct action is:

1. stop implementation planning
2. surface the issue explicitly
3. propose the required steering-document updates
4. treat those updates as proposals until explicitly approved by the user
5. reassess
6. only then continue

## Steering Document Change Rule

The agent must not silently rewrite its own steering constraints as part of implementation work.

If implementation reveals a problem in the current steering documents, the agent must:
- identify the issue explicitly
- explain why the current steering is insufficient or ambiguous
- propose a concrete document update
- treat that update as a proposal until explicitly approved by the user

Implementation must not proceed on the basis of a changed architecture envelope until that change has been explicitly approved and reflected in the steering documents.

Readability refactors performed under the **Readable method bodies** tier (see **In-Code Documentation Requirements** below) do not require a steering-document update, because they are bounded to the allowances under **Readability refactors allowed during documentation passes**; any refactor outside those allowances is an architecture change and must follow the normal escalation process.

## Core Working Mode

Work must proceed in narrow, well-defined, preferably testable phases.

Every phase must be small enough that:
- its goal is clear
- its out-of-scope boundaries are clear
- its risks are reviewable
- its result can be validated against steering documents

Implementation is not allowed to silently broaden the project.

## Mandatory Phase Loop

## Before a phase

Before implementation begins, do all of the following:

1. define narrow scope
2. define explicit out-of-scope
3. run planning mode
4. identify:
   - gaps
   - ambiguities
   - hidden assumptions
   - risks
   - likely technical debt
5. if any of those are found, propose steering-doc updates first
6. treat those updates as pending until explicitly approved by the user
7. reassess whether the phase is now stable
8. only then switch to implementation mode

## After a phase

After implementation, do all of the following:

1. validate against `PROJECT_BRIEF.md`
2. validate against `docs/ARCHITECTURE_PRINCIPLES.md`
3. validate against `docs/VALIDATION_CHECKLIST.md`
4. state what remains weak, risky, or provisional
5. state what must change before the next phase
6. propose document updates if needed before proceeding

## The Agent Must Not

The agent must not:

- invent architecture outside documented scope
- silently broaden scope
- introduce a new subsystem without explicit approval
- introduce a new abstraction layer without explicit approval
- introduce a new persistent state owner without explicit approval
- introduce threading or background work without explaining why and the synchronization model
- move logic into the audio callback without explicit justification
- optimize preemptively without documented reason
- choose major buffer, timing, or transport semantics on its own if they influence future architecture
- merge distinct responsibilities just because it is faster to implement that way
- treat a temporary implementation shortcut as acceptable if it creates hidden architectural debt

## Required Planning Output Before Implementation

Before each phase is implemented, the planning step must produce:

### 1. Phase scope
A short statement of exactly what is being built now.

### 2. Out-of-scope list
A short list of things that are explicitly not being built in this phase.

### 3. Gap and risk analysis
A short list of:
- unclear areas
- assumptions
- likely failure modes
- architectural risks
- possible debt introduced by the proposed approach

### 4. Proposed implementation shape
A short explanation of:
- which responsibilities will exist
- which classes/files are expected to change
- why this split is appropriate for this phase

### 5. Validation plan
A short explanation of how the phase result will be checked.

If this planning output is not clear, implementation should not begin.

## Required Implementation Outputs Per Phase

Every phase must leave behind outputs that make the repository understandable without reading every line of code.

At minimum, provide:

### 1. Short flow map
A short explanation of how control and data move through the new implementation.

Example questions it should answer:
- what triggers what?
- what calls what?
- what state changes where?
- what feeds playback?
- what drives rendering?

### 2. Responsibility map
A short list of new or modified classes/files and what each one is responsible for.

### 3. “Why this split” explanation
A short explanation of why responsibilities were split this way rather than merged or split differently.

### 4. Next-phase support note
A short note explaining how the chosen design supports the next likely phase.

## Required Explicit Questions

For every phase, the agent must explicitly answer:

### What are the plausible wrong implementations that might appear to work?
This should identify deceptive implementations that would seem functional but be architecturally dangerous.

### What assumptions are being made implicitly?
This should surface assumptions that may otherwise remain hidden.

### What decisions are being deferred, and is that safe?
This should make deferred design choices explicit and justify why deferral is acceptable.

## DAW-Specific Implementation Guardrails

Implementation must actively guard against the following:

- hidden coupling between UI and engine
- audio-thread unsafe behavior
- unclear ownership and lifetimes
- duplicated transport state
- file import/decoding mixed with playback logic
- waveform rendering mixed with playback logic
- architecture that only works for one clip and does not scale
- premature abstractions or subsystems outside current scope

## Phase 1 Interpretation Rule

Phase 1 must not be implemented as “just a WAV player”.

It must be implemented as:

**a small timeline/playback engine that currently happens to support one audio clip**

That means Phase 1 should already preserve conceptual separation between:
- UI
- transport
- playback/engine concerns
- file loading/import
- waveform rendering
- clip/session representation

But it must do so without introducing unnecessary general-purpose DAW subsystems.

## Change Discipline

Prefer small, reviewable changes.

Do not mix all of the following in one step unless clearly justified:
- architecture changes
- playback changes
- file-loading changes
- UI redesign
- waveform logic changes
- transport semantics changes

Small phases are preferred over broad rewrites.

## C++ and Code Clarity

Modern C++ should be used when it materially improves:
- correctness
- clarity
- maintainability
- architectural fit

Do not avoid newer language features merely because they are newer.

However:
- do not optimize for cleverness
- prefer readable ownership and lifetime models
- prefer explicitness over hidden magic

The goal is not “minimal C++ sophistication”.  
The goal is modern, clear, learnable, and robust code.

Code-level documentation requirements (file headers, class and method doc comments, audio-thread markers, JUCE-usage notes, and readable method bodies) are mandatory for central source files. The full rule set is in **In-Code Documentation Requirements** below. Those requirements are not optional stylistic advice; they are part of the implementation contract.

## In-Code Documentation Requirements

The **code** is part of the documentation. A reader who knows C++ but does not already know JUCE and does not know this repository must be able, from names, structure, and the comments required here, to understand what each *central* source file and public class is for, who owns what, on which thread code runs, and what non-trivial and thread-sensitive methods *mean* in terms of role and contract—without having to read every line of the implementation for context.

"Central" means hand-written application and domain code under the project's main source tree (for example `src/`), not third-party, generated, or vendored code.

### What comments must and must not do

- Comments must state **role**, **responsibility**, **ownership**, **lifetime**, **threading**, or **realtime/audio-callback constraints** when those are not obvious from the name alone.
- Comments must not narrate syntax ("increment the counter", "assign to x") or restate the identifier name without adding information.
- A TODO or FIXME is not a substitute for a class or method contract where the rubric below requires one.

### The six documentation tiers

These tiers are **mandatory** for the scope described. Trivial one-line getters or obvious accessors do not need a method-level comment unless the contract is non-obvious (for example, realtime safety). Tier 6 applies to method *bodies*; its detailed rules and the allowance for small readability refactors are in **Readable method bodies** and **Readability refactors allowed during documentation passes** below.

1. **File header** — At the top of each central `*.h` and `*.cpp` file, a short block (roughly 3–10 lines) that states: the file's **role in the system**; which architectural concept it implements (for example, transport, sole audio callback, file decode); which **thread(s)** may run its code; and, where relevant, **key collaborators** (as non-owning references vs owned data).

2. **Class documentation** — Before each public `class` (or primary free-function module if the file is intentionally not class-based), a short block (roughly 3–8 lines) that states: what the type **owns** and what it is **responsible** for; **ownership and lifetime** of instances; **threading** (which methods are message-thread-only, which may run on the audio thread, and who is the single authoritative writer of shared state); and, when it would be easy to assume wrongly, what this type **deliberately does not** do.

3. **Method and function documentation** — For every **non-trivial** public method and for every function that is **thread-sensitive** (including anything invoked from or in the same call chain as the audio callback), a short block (roughly 1–5 lines) that states: the **contract** in domain terms; **which thread** may call it; and, for code on or reachable from the realtime path, that it does **not** allocate, block, or perform I/O on that path (unless the steering documents explicitly allow an exception, which must be named).

4. **Audio-thread markers** — Any function that **runs on** the audio device callback thread, or is **intended to be called only** from that thread, must carry an explicit label in a comment (for example `// [Audio thread]`) and a one-line statement of the forbidden operations on that path (no allocation, locks, waits, I/O, UI) unless a documented exception applies.

5. **JUCE-usage notes** — Where a line uses a JUCE API whose *purpose* is not obvious to someone who does not know JUCE (for example, a non-obvious `FloatVectorOperations` use, a reader with unusual lifetime semantics, or a flag on a callback), add a **one-line** note explaining what JUCE is doing *here* and *why*. Obvious types (`juce::File`, `juce::String`, basic `Component` wiring) do not need a per-line note.

6. **Body readability** — The *body* of every central method in scope (non-trivial public methods, and every function on or reachable from the audio callback) must satisfy **Readable method bodies** below, including **in-body explanatory comments** where a branch's or operation's meaning in system or product terms would otherwise be hidden behind pointer arithmetic, channel indexing, buffer-tail handling, or JUCE mechanics. The reviewer judges on *outcome* (can a C++-fluent reader who does not know JUCE or this codebase follow the main path *and the meaning of its branches* from comments and local names alone?), not on comment count or chunk cadence.

### Readable method bodies

**Readable method bodies.** In addition to the header/class/method doc tiers, the *body* of every central method — non-trivial public methods and every function on or reachable from the audio callback — must be readable top-down. A reader who knows C++ but not JUCE and not this codebase must be able to follow the main path of the method by reading the comments and local variable names alone, without decoding pointer arithmetic, channel indexing, buffer-tail handling, or JUCE internals to understand intent.

**Readability is the goal. Chunking is a tool, not a ritual:**

- Where a body has multiple conceptual steps, split it into logical chunks (typically 3–15 lines each) and precede each chunk with a 1–3 line plain-language comment that states *intent* (why this chunk exists and what it does in domain terms), not a line-by-line narration of syntax.
- Where a short body already reads top-down clearly with good names and a single leading comment, that is sufficient — do not insert chunk breaks just to hit a cadence.
- Use local names that describe the role of the value, not only its low-level mechanism. Prefer `monoSource`, `framesToPlay`, `blockSize`, `playheadOffsetInSamples`, `outChannel` over `src`, `advance`, `n`, `offset`, `out`.
- If a dense or repeated block still requires reverse-engineering after naming and chunk comments, consider a small private helper under **Readability refactors allowed during documentation passes** below; that is an option, not an obligation. Do not extract helpers just to satisfy the rubric formally.
- Where the meaning of a branch or operation in *system or product terms* is not obvious from names and structure, add a short **in-body explanatory comment** (typically 1–2 lines) that states that meaning in plain language. Examples of the level intended: "Phase 1 mono-to-stereo special case: duplicate the mono clip channel to left and right.", "Copy only the playable part of the clip for this block.", "Clear the unused tail so the device buffer is fully defined.", "Pending seek is applied here so playhead and audio stay aligned for this block." These comments explain *what this code means for the system*, not what the C++ does; pointer arithmetic, channel indexing, buffer-tail handling, and JUCE-specific mechanics are the typical triggers.
- Narration that only restates C++ syntax or re-describes the identifier name does not count as a chunk comment and violates the rubric.

**What an in-body explanatory comment is (and is not).** It is a short plain-language statement of a branch's or operation's role in the system or product — a domain- or rule-level sentence such as "this is the Phase 1 mono-to-stereo rule" or "we clear the tail so the device buffer is fully defined." It is *not* a restatement of the call being made, not a narration of pointer arithmetic or loop indices, and not a substitute for the method-level doc comment (which describes the *contract*). These comments are additive to chunk labels and role-named locals; they are required whenever a reader who knows C++ but not JUCE and not this codebase could not otherwise understand what a branch means in system or product terms. Use them sparingly and where they actually help; do not paraphrase obvious code.

### Readability refactors allowed during documentation passes

A documentation pass may perform small, bounded refactors when they are *necessary* to satisfy the body-readability tier. Refactors in this category must preserve observable behavior and must be listed in the commit/PR note.

**Allowed**

- Renaming local variables and parameters to role-describing names (for example `src` → `monoSource`, `offset` → `playheadOffsetInSamples`, `out` → `outChannel`, `framesToAdvance` → `framesToPlay`). Renames of parameter names in headers must not change a public symbol in a way that breaks callers; public method *signatures* are not changed.
- Extracting a small private helper function (typically 5–20 lines, in the same translation unit, as a `static` function or `private` method) **when it genuinely improves readability** — for example, replacing a dense, repeated, or multi-responsibility block with a named operation whose intent is clear from the signature. Helper extraction is optional, not required in every file. Example: a helper that centralizes a copy-then-silence-tail pattern that would otherwise be duplicated. Do not extract helpers only to check a box.
- Introducing a named local `const` for a magic expression (for example `const int tailSamples = blockSize - framesToPlay;`).
- Reordering independent statements where the new order improves top-down readability and does not alter semantics (dependency-preserving reordering only).
- Replacing a nested `if`/`else` with early-return guards where the resulting control flow is semantically identical.
- Splitting one long function into a short top-level function plus a small private helper, provided the helper is a trivial extraction of an existing block, not a new abstraction.

**Forbidden**

- Any change to **public API** (signatures, names, const-ness as observed by callers) except renames of **local** names where the public symbol is unchanged.
- Any change to **threading or realtime contracts** (for example moving work into or out of the audio callback).
- Any change to **observable behavior**, including audio output, timing, end-of-buffer semantics, seek timing, error surfaces, or the set of edge cases handled.
- Introducing **new types**, **new public classes**, **new files**, **new subsystems**, **new allocations on the realtime path**, or new JUCE APIs beyond what was already used.
- Generalizing Phase-1 rules (for example turning the mono-to-stereo special case into a general upmix path). The rule stays as-written; only its *expression* in code may become more readable.
- "Performance" refactors, style-only refactors unrelated to readability, or swapping data structures.

**Reviewer check.** A readability refactor is acceptable only if the reviewer can answer *yes* to all of: (a) behavior is identical (for example, manual check against the prior version), (b) the refactor's scope is listed in the commit, (c) no public API or thread contract has changed in a forbidden way, (d) after the refactor, the central method body passes the body-readability outcome above.

### Exemptions

The following are **exempt** from the file-header and class/method doc tiers unless a phase explicitly brings them in scope: `CMakeLists.txt`, build and packaging scripts, and other non-`src/` automation. Third-party and generated code is exempt.

### Validation gate (hard)

A phase is **not** considered complete for validation purposes if any **central source file** that was **added or meaningfully changed** in that phase is left in violation of the six tiers above. Touching a file in passing (for example, a one-line import path fix) should either bring the file up to the rubric for the parts of the file in scope, or the phase report must explicitly list the file and justify minimal touch—but **new** or **substantially extended** code must comply fully.

## Escalation Rule

If implementation pressure reveals that the documented architecture is missing something important, do not patch around the problem informally.

Instead:
- pause
- document the missing architectural issue
- propose the relevant steering-document update
- wait for explicit user approval
- reassess the phase plan
- then continue
