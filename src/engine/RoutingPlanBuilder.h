#pragma once

#include "engine/RoutingPlan.h"

#include <memory>

class SessionSnapshot;

namespace routing_plan_builder
{

/// [Message thread] Build an immutable plan from `snap`. `busScratchPairs[i]` = `{L,R}` write pointers for bus `i`.
[[nodiscard]] std::shared_ptr<const RoutingPlan> build(
    const SessionSnapshot& snap,
    const std::vector<std::pair<float*, float*>>& busScratchPairs) noexcept;

} // namespace routing_plan_builder
