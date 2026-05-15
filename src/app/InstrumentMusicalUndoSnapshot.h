#pragma once

#include <functional>
#include <vector>

#include "domain/SessionSnapshot.h"
#include "io/ProjectFile.h"

class InstrumentTrackController;

namespace mini_daw_app_transport
{

struct InstrumentMusicalUndoSnapshotCallbacks
{
    std::function<InstrumentTrackController*(TrackId)> getInstrumentControllerForTrack;
};

[[nodiscard]] std::vector<ProjectFileExperimentalInstrumentTrackV1>
buildSortedInstrumentMusicalUndoSnapshot(const SessionSnapshot& snapshot,
                                         const InstrumentMusicalUndoSnapshotCallbacks& callbacks);

void stableSortInstrumentMusicalUndoVector(std::vector<ProjectFileExperimentalInstrumentTrackV1>& rows);

} // namespace mini_daw_app_transport
