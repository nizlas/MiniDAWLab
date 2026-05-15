#include "app/InstrumentMusicalUndoSnapshot.h"

#include <algorithm>

#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"

std::vector<ProjectFileExperimentalInstrumentTrackV1>
mini_daw_app_transport::buildSortedInstrumentMusicalUndoSnapshot(
    const SessionSnapshot& snapshot,
    const InstrumentMusicalUndoSnapshotCallbacks& callbacks)
{
    std::vector<ProjectFileExperimentalInstrumentTrackV1> out;
    for (int ti = 0; ti < snapshot.getNumTracks(); ++ti)
    {
        const Track& tr = snapshot.getTrack(ti);
        if (tr.getKind() != TrackKind::Instrument)
        {
            continue;
        }
        InstrumentTrackController* const ctl = callbacks.getInstrumentControllerForTrack(tr.getId());
        if (ctl == nullptr || !ctl->hasInstrumentTrack()
            || ctl->getExperimentalInstrumentDomainTrackId() != tr.getId())
        {
            continue;
        }
        std::vector<ProjectFileExperimentalInstrumentTrackV1> one = ctl->buildExperimentalInstrumentMusicalUndoBlock();
        out.insert(out.end(), std::make_move_iterator(one.begin()), std::make_move_iterator(one.end()));
    }
    mini_daw_app_transport::stableSortInstrumentMusicalUndoVector(out);
    return out;
}

void mini_daw_app_transport::stableSortInstrumentMusicalUndoVector(
    std::vector<ProjectFileExperimentalInstrumentTrackV1>& v)
{
    std::stable_sort(
        v.begin(),
        v.end(),
        [](const ProjectFileExperimentalInstrumentTrackV1& a,
           const ProjectFileExperimentalInstrumentTrackV1& b) noexcept -> bool { return a.trackId < b.trackId; });
}
