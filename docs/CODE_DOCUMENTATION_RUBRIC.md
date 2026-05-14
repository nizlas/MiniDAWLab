# Code documentation rubric (in-code)

This file is the **single authoritative** definition of **In-Code Documentation Requirements** for MiniDAWLab central source under `src/`.

Phase workflow, escalation, and “must not” lists remain in [`IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md). When you edit central code or complete a documentation / readability slice, apply this rubric end-to-end.

---

## In-Code Documentation Requirements

The **code** is part of the documentation. A reader who knows C++ but does not already know JUCE and does not know this repository must be able, from names, structure, and the comments required here, to understand what each *central* source file and public class is for, who owns what, on which thread code runs, and what non-trivial and thread-sensitive methods *mean* in terms of role and contract—without having to read every line of the implementation for context.

"Central" means hand-written application and domain code under the project's main source tree (for example `src/`), not third-party, generated, or vendored code.

### What comments must and must not do

- Comments must state **role**, **responsibility**, **ownership**, **lifetime**, **threading**, or **realtime/audio-callback constraints** when those are not obvious from the name alone.
- Comments must not narrate syntax ("increment the counter", "assign to x") or restate the identifier name without adding information.
- A TODO or FIXME is not a substitute for a class or method contract where the rubric below requires one.

### Pedagogical explanation is the default for central code

For central source files, pedagogical explanation is the default, not a special extra added only for unusually difficult code.

The implementer must assume that a reader needs help understanding:
- why a function exists
- what role a helper plays
- what a branch means in system/product terms
- why a particular rendering/playback/transport rule exists
- what is deliberately being communicated or suppressed

Do not optimize for the minimum comment level that can be defended formally.
Optimize for immediate human understanding.

If a helper, branch, or local block would be clearer to a human with a short plain-language explanation, that explanation should be present.

### Minimum-defensible documentation is not sufficient

Documentation is not considered adequate merely because a reviewer can technically reconstruct the meaning from code, names, and a few comments.

The standard is higher:
important logic should be understandable without substantial reverse-engineering.

If a human reader would still need to mentally simulate control flow, decode coordinate math, or infer product meaning from low-level operations, the documentation is still insufficient even if comments are present.

### How central code should read

Central code should read top-down like an explanation of the system, not like a puzzle with technical comments attached.

For a non-trivial central method or helper, a reader should be able to understand, in order:
- why this function exists
- what role it plays in the system
- what the main phases of the function are
- what each important branch means in system/product terms
- what important coordinate-space, time-space, or state-space distinctions are being preserved
- what is deliberately not being done, where that would otherwise be easy to misread

This usually requires, where relevant:
- a short role comment before the function
- conceptual chunking inside the body
- role-describing local names
- plain-language comments for important branches and transformations
- explicit explanation when the code moves between different spaces or interpretations (for example material space vs timeline space, visible vs covered regions, transport state vs rendered state)

The goal is not “some comments”.
The goal is that a reader does not need substantial reverse-engineering to understand the function.

A function is still under-documented if the reader must mentally reconstruct the architecture, infer product meaning from math or framework calls, or simulate control flow in detail before the role of the code becomes clear.

### Example of the intended pedagogical level

The intended standard is not merely that comments exist, but that a central function becomes understandable top-down without substantial reverse-engineering.

Too weak:
- brief technical labels
- unexplained short local names
- comments that only describe mechanics
- no clear statement of why the function exists
- no clear separation of conceptual phases
- no explicit explanation of important distinctions such as material-space vs timeline-space or visible vs covered regions

Acceptable:
- the function first states its role in the system
- the body is divided into meaningful conceptual chunks
- local names describe the role of the values
- comments explain the system/product meaning of important branches
- comments explain important transformations between spaces or interpretations
- a reader can understand why the function exists and how it fits into the system without reconstructing that meaning from low-level operations

Example sketch (illustrative only, not a required exact style):

```cpp
// [Message thread] Build a cheap drawing cache for the current snapshot.
// This function does not decide playback or overlap coverage.
// Its only job is to turn clip material into a small set of peak columns
// that paint() can draw quickly.
void rebuildPeaksIfNeeded()
{
    const auto snapshot = session_.loadSessionSnapshotForAudioThread();
    const int componentWidth = juce::jmax(1, getWidth());

    // If the immutable snapshot and width are unchanged, the existing cache is still valid.
    if (snapshot.get() == lastSnapshotKey_ && componentWidth == lastWidth_)
    {
        return;
    }

    lastSnapshotKey_ = snapshot.get();
    lastWidth_ = componentWidth;
    clipStrips_.clear();

    // No snapshot content means there is nothing to draw.
    if (snapshot == nullptr || snapshot->isEmpty())
    {
        return;
    }

    const std::int64_t timelineEndExclusive = snapshot->getDerivedTimelineLengthSamples();
    if (timelineEndExclusive <= 0)
    {
        return;
    }

    // Build one cached strip per placed clip.
    for (int clipIndex = 0; clipIndex < snapshot->getNumPlacedClips(); ++clipIndex)
    {
        // ...
        // Important: peak extraction happens in material space [0, N).
        // Timeline placement is handled later in paint() using startOnTimeline.
        // ...
    }
}
```

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

### Internal helpers are not “lower standard” code

Internal helper functions in central files must meet the same pedagogical standard as larger methods when they carry meaningful system/product behavior.

A helper is in this category if it:
- implements a visible UI rule
- implements playback, transport, ordering, coverage, overlap, or visibility behavior
- performs mapping between timeline/domain state and rendered output
- encodes a rule that would otherwise be hidden behind math, buffer logic, or framework calls

Such helpers must explain in plain language:
- why they exist
- what user-visible or system-visible rule they implement
- what they are intentionally not doing, if that is easy to misread


### Examples and anti-examples are normative

When body readability is reviewed, both positive examples and anti-examples are part of the rule.

A method body does **not** satisfy this rule merely because it has:
- a strong file header
- better variable names
- structural chunk labels
- or short technical comments

The rule is satisfied only when important branches are explained in plain language at the level of system/product meaning where needed.

**Anti-example**
- "Mono-to-stereo branch."  
  This is too weak if the reader still has to decode low-level code details to understand what the rule means.

**Acceptable example**
- "Phase 1 mono-to-stereo product rule: a mono clip should be heard on both left and right speakers on a stereo device, so we duplicate the same mono source channel to outputs 0 and 1."

Reviewers should reject documentation that is formally commented but still requires the reader to infer branch meaning from low-level mechanics.

### New central files and important internal helpers are not exempt

The documentation bar applies immediately to **new central files** introduced in a phase.
A new domain, engine, UI, transport, or session file under `src/` must not be treated as “too new”, “too small”, or “obviously understandable” as a reason to defer pedagogical documentation to a later step.

If a phase introduces a new central concept, the file(s) that define and implement that concept must already explain:
- why the concept exists
- how it fits into the architecture
- what it owns or represents
- how it participates in ownership, lifetime, and threading
- what is transitional or phase-specific about its current role, if applicable

Likewise, an internal helper function is **not** exempt merely because it is:
- in an anonymous namespace
- `static`
- private
- short
- or only used from one method

If that helper carries important **system/product meaning** that would otherwise remain hidden behind rendering/math/buffer mechanics, it must still be documented in plain language at the level needed for a reader to understand why it exists and what role it plays.

Reviewers should reject documentation that is strong only on large public methods while leaving new central files or important internal helpers under-explained.

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

### Documentation-pass review output

A documentation pass is not reviewable from summary alone.

For each documentation slice, the implementation report must include:
- the full text of at least one central updated method body
- at least one updated branch or helper example that demonstrates improved plain-language readability
- a brief note explaining what system/product meaning is now explicit that previously had to be inferred from mechanics

A summary such as “documentation was improved”, “comments were added”, or “body readability was strengthened” is not sufficient on its own.

If a new central file was introduced or substantially changed in the slice, the report should preferentially include an example from that file.
