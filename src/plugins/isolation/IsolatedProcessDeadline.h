#pragma once

// One absolute deadline per processing context (plan D.3). Every isolated-stage wait in a device
// callback, in any lane, waits only until the same absolute time; read-ahead sets its own per
// segment on its thread; offline export has none and waits in slices instead.

#include <atomic>
#include <cstdint>

namespace mini_daw::isolation::deadline
{
[[nodiscard]] std::int64_t qpcNow() noexcept;
[[nodiscard]] std::int64_t qpcPerSecond() noexcept;

/// [Audio thread] At the start of each device callback: start + budget − reserve.
void publishCallbackDeadline(std::int64_t qpcDeadline) noexcept;

/// [Read-ahead thread] Deadline for the segment this thread is about to render; 0 clears it.
void setThreadDeadline(std::int64_t qpcDeadline) noexcept;

/// The deadline that applies to the calling thread: its own, else the current callback's.
[[nodiscard]] std::int64_t currentDeadline() noexcept;

/// [Message thread] Offline export running: isolated stages wait without a real-time deadline
/// (sliced, bounded by helper liveness and progress) and report failures instead of silence.
void setOfflineRenderActive(bool active) noexcept;
[[nodiscard]] bool offlineRenderActive() noexcept;
/// Set by a stage that could not produce its block during offline export; cleared by the exporter.
void noteOfflineFailure() noexcept;
[[nodiscard]] bool takeOfflineFailure() noexcept;
} // namespace mini_daw::isolation::deadline
