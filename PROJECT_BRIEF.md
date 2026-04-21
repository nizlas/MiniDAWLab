# MiniDAWLab — Scope & Working Agreement

## Purpose

This project is primarily a hobby and learning project for better understanding:

- audio engines
- DAW architecture
- routing
- playback
- MIDI
- GUI
- plugin-related infrastructure

The project is **not primarily intended to replace Cubase**.  
If it eventually grows into a streamlined tool that can compete with Cubase for our own workflow, that is a bonus rather than the core requirement.

## Relationship to Cubase

Cubase will remain the main environment for real-world recording and production for the foreseeable future.

The MiniDAW project is intended for:

- learning
- experimentation
- architectural understanding
- possible development of a narrower custom workflow tool

## Overall Product Idea

The goal is a focused mini-DAW for our own way of working, not a general-purpose full DAW.

Over time, it should be able to support:

- audio on a timeline
- MIDI for melody prototyping
- a limited number of relevant instruments
- simple but good manual audio editing
- inserts, sends, and group buses
- a workflow that is faster and cleaner for our own needs

## Primary Technical Stack

- **JUCE** for GUI, audio, and general application structure
- **Cursor** as the primary development environment
- **CMake** as the overall build system
- **Ninja** as the likely default build generator
- **MSVC toolchain + Windows SDK** on Windows
- **Visual Studio / Build Tools** installed as the underlying build and fallback environment when needed

## Working Principle

The default is to work in Cursor, while using Microsoft’s build tools under the hood on Windows.

Visual Studio is available as a fallback and for situations where its debugging or toolchain support is useful, but it is **not** intended to be the primary working environment.

## Product Scope

This project should stay narrow and focused.

We are not trying to build “Cubase Lite” as a goal in itself, but rather a tool that grows organically from simple but real needs.

## Important Use Cases

### MIDI

We need MIDI because we prototype melodies in a MIDI editor.

### Instrument Focus

The instruments that are actually relevant for our workflow are mainly:

- drums, typically via Groove Agent SE5
- organ, for example via HALion Sonic
- occasional cello
- the occasional synth sound
- grand piano

### Audio Focus

Relevant audio sources include:

- vocals
- electric guitar, often via Amplitube
- electric bass
- acoustic guitar

### Mix and Routing Needs

Over time, the system should support:

- sends to shared reverb / FX buses
- group buses for shared volume control of multiple tracks

This means the routing architecture should eventually be able to support:

- audio tracks
- MIDI / instrument tracks
- FX buses
- group buses
- master bus

## What We Do Not Need in an Early Version

We do **not** need advanced comping in v1.

Our common workflow is often manual:

- record several tracks
- cut manually
- make fades manually
- sometimes keep two tracks for the same guitar instrument
- manually choose and shape the final result

Because of that, it is enough for an early version to support:

- good manual clip editing
- trim
- split
- fades
- crossfades
- drag/drop of clips

## First Clear Milestone

The first reasonable goal is:

**Load an audio event / audio file and play it back correctly through the audio interface in a scaffolding architecture that can grow into a mini-DAW.**

This should ideally include:

- open audio file
- show simple waveform
- play / stop / pause
- playback through audio interface
- seek / playhead
- simple transport
- clear separation between UI, transport, and audio engine

## Architectural Principle for the First Step

The first version should **not** be seen as “an app that only plays a wav file”, but rather as:

**a small timeline / playback engine that happens to begin with a single audio clip**

The architecture should therefore be able from the beginning to grow toward:

- multiple clips
- multiple tracks
- mixer
- MIDI
- routing
- sends and group buses

## Must-Haves in the Early Direction

- audio playback on a timeline
- clean separation between engine and UI
- ability to grow forward logically
- solid foundation for waveform rendering and transport

## Things That Can Wait

- advanced comping
- broad general DAW functionality
- notation / score
- surround
- film / video
- large routing matrix
- full Cubase-like mixer from the start
- maximal plugin breadth from day one

## Strategic Direction

The project should grow step by step:

1. audio file on timeline
2. stable playback through audio interface
3. multiple clips
4. multiple tracks
5. simple mixer
6. routing, sends, and groups
7. MIDI clips
8. piano roll
9. instrument tracks
10. more complete mini-DAW functionality

## Important Boundary

We are not trying to win by building everything.

We are trying to win by building exactly what we actually care about.
