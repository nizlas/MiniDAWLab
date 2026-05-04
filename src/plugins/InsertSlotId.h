#pragma once

// =============================================================================
// InsertSlotId — stable id for one insert slot on a track (insert-chain groundwork)
// =============================================================================

#include <cstdint>

using InsertSlotId = std::uint64_t;

inline constexpr InsertSlotId kInvalidInsertSlotId = 0;

enum class InsertStage
{
    Pre,
    Post,
};
