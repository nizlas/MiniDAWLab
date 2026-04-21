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
- briefly explain less obvious constructs when that helps understanding

The goal is not “minimal C++ sophistication”.  
The goal is modern, clear, learnable, and robust code.

## Escalation Rule

If implementation pressure reveals that the documented architecture is missing something important, do not patch around the problem informally.

Instead:
- pause
- document the missing architectural issue
- propose the relevant steering-document update
- wait for explicit user approval
- reassess the phase plan
- then continue
