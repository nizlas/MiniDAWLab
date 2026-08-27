#pragma once

#include "engine/RoutingPlan.h"

#include <memory>

class SessionSnapshot;

namespace routing_plan_builder
{

/// [Message thread] Build an immutable plan from `snap`. `busScratchPairs[i]` = `{L,R}` write pointers for bus `i`.
/// `busScratchOwners` (Stability C4B) carries shared ownership of the buffers behind the pairs so the
/// pointers stay valid for the plan's entire lifetime; it is stored on the plan and never dereferenced.
[[nodiscard]] std::shared_ptr<const RoutingPlan> build(
    const SessionSnapshot& snap,
    const std::vector<std::pair<float*, float*>>& busScratchPairs,
    std::vector<std::shared_ptr<void>> busScratchOwners = {}) noexcept;

} // namespace routing_plan_builder
