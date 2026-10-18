#pragma once

// =============================================================================
// Vst3PresetFile — read-only check of a Steinberg .vstpreset container
// =============================================================================
//
// ROLE
//   Decide whether a byte block is a VST3 preset file DAL is willing to hand to
//   the plug-in. The bytes on disk are the container JUCE 8.0.4 already writes
//   with Steinberg's PresetFile (header 'VST3', version 1, 32-character
//   component class id, 'Comp' processor state, optional 'Cont' controller
//   state). This file does not build that container and it does not turn a
//   JUCE state blob into one.
//
// WHY A CHECK SEPARATE FROM setPreset
//   JUCE's setPreset calls PresetFile::loadPreset, which compares the class id
//   and then calls IComponent::setState. The SDK reader does not reject a chunk
//   whose offset or length falls outside the file before that call, and a later
//   controller-state failure cannot undo a component-state that already landed.
//   DAL therefore refuses a file that fails this check and never calls setPreset.
//
// THREADING
//   [Message thread] only. The parser allocates nothing itself; copyChunk does,
//   into the caller's MemoryBlock. Never call either from the audio callback.
// =============================================================================

#include <juce_core/juce_core.h>

#include <cstdint>
#include <cstring>

namespace vst3_preset
{

/// Largest preset file DAL will read or write. Effect presets are far smaller;
/// the cap is what stops a hostile length from being mapped into a chunk walk.
inline constexpr std::uint64_t kMaxPresetFileBytes = 64ull * 1024ull * 1024ull;

/// Steinberg header: 'VST3' + version + 32-byte class id + list offset.
inline constexpr int kHeaderBytes = 48;

/// PresetFile::kMaxEntries. A larger count is rejected rather than clamped,
/// because clamping would drop entries and still apply what remained.
inline constexpr int kMaxChunkEntries = 128;

enum class Status
{
    Ok,
    TooLarge,
    Truncated,
    BadHeader,
    BadChunk,
    MissingComponentState
};

struct Inspection
{
    Status status = Status::BadHeader;
    /// 32 hexadecimal characters, upper case, when status is Ok. Empty otherwise.
    juce::String classId;
};

[[nodiscard]] inline bool isHexClassId(juce::StringRef id) noexcept
{
    if (id.length() != 32)
    {
        return false;
    }
    for (int i = 0; i < 32; ++i)
    {
        const juce::juce_wchar c = id[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!hex)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool sameClassId(juce::StringRef a, juce::StringRef b) noexcept
{
    if (!isHexClassId(a) || !isHexClassId(b))
    {
        return false;
    }
    for (int i = 0; i < 32; ++i)
    {
        const auto upper = [](const juce::juce_wchar c) noexcept {
            return (c >= 'a' && c <= 'f') ? (juce::juce_wchar)(c - 'a' + 'A') : c;
        };
        if (upper(a[i]) != upper(b[i]))
        {
            return false;
        }
    }
    return true;
}

namespace detail
{

[[nodiscard]] inline std::uint32_t readU32(const std::uint8_t* p) noexcept
{
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16)
           | ((std::uint32_t)p[3] << 24);
}

[[nodiscard]] inline std::int64_t readI64(const std::uint8_t* p) noexcept
{
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i)
    {
        u |= (std::uint64_t)p[i] << (8 * i);
    }
    return (std::int64_t)u;
}

[[nodiscard]] inline bool idIs(const char* p, const char* four) noexcept
{
    return std::memcmp(p, four, 4) == 0;
}

struct ChunkLoc
{
    char id[4] {};
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

/// Shared walk used by inspect and copyChunk so the two cannot disagree.
struct Walk
{
    Inspection inspection;
    ChunkLoc chunks[kMaxChunkEntries] {};
    int chunkCount = 0;
};

[[nodiscard]] inline Walk walk(const void* data, const std::uint64_t size) noexcept
{
    Walk out;
    if (size > kMaxPresetFileBytes)
    {
        out.inspection.status = Status::TooLarge;
        return out;
    }
    if (data == nullptr || size < (std::uint64_t)kHeaderBytes)
    {
        out.inspection.status = Status::Truncated;
        return out;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (!idIs(reinterpret_cast<const char*>(bytes), "VST3"))
    {
        out.inspection.status = Status::BadHeader;
        return out;
    }
    const std::uint32_t version = readU32(bytes + 4);
    if (version != 1u)
    {
        out.inspection.status = Status::BadHeader;
        return out;
    }
    char classRaw[33] {};
    std::memcpy(classRaw, bytes + 8, 32);
    const juce::String classId(classRaw);
    if (!isHexClassId(classId))
    {
        out.inspection.status = Status::BadHeader;
        return out;
    }

    const std::int64_t listOffsetSigned = readI64(bytes + 40);
    if (listOffsetSigned < kHeaderBytes)
    {
        out.inspection.status = Status::BadChunk;
        return out;
    }
    const auto listOffset = (std::uint64_t)listOffsetSigned;
    if (listOffset > size || size - listOffset < 8ull)
    {
        out.inspection.status = Status::Truncated;
        return out;
    }
    if (!idIs(reinterpret_cast<const char*>(bytes + listOffset), "List"))
    {
        out.inspection.status = Status::BadChunk;
        return out;
    }
    const std::uint32_t countRaw = readU32(bytes + listOffset + 4);
    if (countRaw == 0u || countRaw > (std::uint32_t)kMaxChunkEntries)
    {
        out.inspection.status = Status::BadChunk;
        return out;
    }
    const std::uint64_t entriesBytes = (std::uint64_t)countRaw * 20ull;
    if (size - listOffset - 8ull < entriesBytes)
    {
        out.inspection.status = Status::Truncated;
        return out;
    }

    bool sawComponent = false;
    bool sawController = false;
    const auto* entry = bytes + listOffset + 8;
    for (std::uint32_t i = 0; i < countRaw; ++i)
    {
        const auto* row = entry + (std::uint64_t)i * 20ull;
        const std::int64_t offsetSigned = readI64(row + 4);
        const std::int64_t sizeSigned = readI64(row + 12);
        if (offsetSigned < kHeaderBytes || sizeSigned < 0)
        {
            out.inspection.status = Status::BadChunk;
            return out;
        }
        const auto offset = (std::uint64_t)offsetSigned;
        const auto chunkSize = (std::uint64_t)sizeSigned;
        // Chunk bytes live in the data area, strictly before the chunk list.
        if (offset > listOffset || listOffset - offset < chunkSize)
        {
            out.inspection.status = Status::BadChunk;
            return out;
        }
        ChunkLoc loc;
        std::memcpy(loc.id, row, 4);
        loc.offset = offset;
        loc.size = chunkSize;
        if (idIs(loc.id, "Comp"))
        {
            if (sawComponent)
            {
                out.inspection.status = Status::BadChunk;
                return out;
            }
            sawComponent = true;
        }
        else if (idIs(loc.id, "Cont"))
        {
            if (sawController)
            {
                out.inspection.status = Status::BadChunk;
                return out;
            }
            sawController = true;
        }
        out.chunks[out.chunkCount++] = loc;
    }
    if (!sawComponent)
    {
        out.inspection.status = Status::MissingComponentState;
        return out;
    }
    out.inspection.status = Status::Ok;
    out.inspection.classId = classId.toUpperCase();
    return out;
}

} // namespace detail

[[nodiscard]] inline Inspection inspect(const void* data, const std::uint64_t size) noexcept
{
    return detail::walk(data, size).inspection;
}

/// Copies one chunk ('Comp', 'Cont', …) out of a file that inspect() accepts.
/// Returns false without writing `out` when the file is rejected or the id is absent.
[[nodiscard]] inline bool copyChunk(const void* data,
                                    const std::uint64_t size,
                                    const char id[4],
                                    juce::MemoryBlock& out)
{
    const detail::Walk walked = detail::walk(data, size);
    if (walked.inspection.status != Status::Ok || data == nullptr || id == nullptr)
    {
        return false;
    }
    for (int i = 0; i < walked.chunkCount; ++i)
    {
        if (std::memcmp(walked.chunks[i].id, id, 4) == 0)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            out.replaceAll(bytes + walked.chunks[i].offset, walked.chunks[i].size);
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline juce::String messageFor(const Status status)
{
    switch (status)
    {
        case Status::Ok: return {};
        case Status::TooLarge: return "The preset file is larger than 64 MB. Nothing was changed.";
        case Status::Truncated: return "The preset file is incomplete. Nothing was changed.";
        case Status::BadHeader: return "This file is not a VST3 preset. Nothing was changed.";
        case Status::BadChunk: return "The preset file's contents are damaged. Nothing was changed.";
        case Status::MissingComponentState:
            return "The preset file has no plug-in state. Nothing was changed.";
    }
    return "This file is not a VST3 preset. Nothing was changed.";
}

} // namespace vst3_preset
