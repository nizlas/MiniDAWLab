#pragma once

// Control-channel records for the isolated insert helper (see IsolatedProtocol.h). Message thread
// and pipe threads only: these allocate and must never be used on an audio lane.

#include <juce_core/juce_core.h>

#include "plugins/isolation/IsolatedProtocol.h"

namespace mini_daw::isolation
{
class ControlWriter
{
public:
    ControlWriter(const ControlKind kind, const std::int32_t requestId)
    {
        out_.writeInt((int)kind);
        out_.writeInt(requestId);
    }
    ControlWriter& i32(const std::int32_t v) { out_.writeInt(v); return *this; }
    ControlWriter& i64(const std::int64_t v) { out_.writeInt64(v); return *this; }
    ControlWriter& f64(const double v) { out_.writeDouble(v); return *this; }
    ControlWriter& str(const juce::String& s) { out_.writeString(s); return *this; }
    ControlWriter& bytes(const void* data, const size_t size)
    {
        out_.writeInt64((juce::int64)size);
        if (size > 0)
        {
            out_.write(data, size);
        }
        return *this;
    }
    ControlWriter& bytes(const juce::MemoryBlock& b) { return bytes(b.getData(), b.getSize()); }
    [[nodiscard]] juce::MemoryBlock block() const { return out_.getMemoryBlock(); }

private:
    juce::MemoryOutputStream out_;
};

class ControlReader
{
public:
    explicit ControlReader(const juce::MemoryBlock& m) : in_(m, false)
    {
        kind_ = (ControlKind)in_.readInt();
        requestId_ = in_.readInt();
    }
    [[nodiscard]] ControlKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::int32_t requestId() const noexcept { return requestId_; }
    std::int32_t i32() { return in_.readInt(); }
    std::int64_t i64() { return in_.readInt64(); }
    double f64() { return in_.readDouble(); }
    juce::String str() { return in_.readString(); }
    juce::MemoryBlock bytes()
    {
        const juce::int64 n = in_.readInt64();
        juce::MemoryBlock b;
        if (n > 0 && n <= in_.getNumBytesRemaining())
        {
            b.setSize((size_t)n);
            in_.read(b.getData(), (int)n);
        }
        return b;
    }

private:
    juce::MemoryInputStream in_;
    ControlKind kind_ = ControlKind::Ping;
    std::int32_t requestId_ = 0;
};
} // namespace mini_daw::isolation
