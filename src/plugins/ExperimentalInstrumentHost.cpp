// =============================================================================
// ExperimentalInstrumentHost.cpp — per-`TrackId` hosted instrument (VST3); app holds one host instance per instrument lane
// =============================================================================
//
// Include juce_audio_basics before our header so MidiBuffer/Message resolve before
// juce_audio_processors + VST3 SDK (can confuse MSVC's `juce::` lookup).
// MIDI from UI uses CriticalSection + MidiBuffer, not MidiMessageCollector (same SDK issue).
// =============================================================================

#include <juce_audio_basics/juce_audio_basics.h>
#include "plugins/ExperimentalInstrumentHost.h"

#include "domain/TrackStereoPan.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <set>
#include <vector>

#include "diagnostics/DrumNameDiagnosticConfig.h"
#include "diagnostics/DiagnosticBuildFlags.h"
#include "diagnostics/ExperimentalPlaybackRoutingLog.h"
#include "diagnostics/DrumNameDiagnosticFileLog.h"
#include "plugins/Vst3ChildProcessScan.h"
#include "ui/experimental/DrumNoteNames.h"

#if JUCE_PLUGINHOST_VST3 && (JUCE_WINDOWS || JUCE_MAC || JUCE_LINUX || JUCE_BSD)
// juce_audio_processors.cpp already compiles the Steinberg SDK .cpp sources; including the default
// juce_VST3Headers.h here would paste those implementations again → LNK2005 multiply-defined symbols.
#define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
#include <juce_audio_processors/format_types/juce_VST3Headers.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#endif

namespace
{
/// When true, writes global drum `<capabilities>` into v2 after runtime name harvest. **Default false:**
/// global cached drum maps are not kit/preset canonical and are not used for production drum-row display;
/// the intended model is per-instrument-track / project-local (future work).
constexpr bool kPersistGlobalDrumCapabilityHints = false;

/// Production does **not** auto-harvest plugin pitch/pad names into MIDI editor drum rows on load, rescan,
/// cached load, or native editor open. Plugin-reported names may later be imported via an explicit UI action;
/// until then `pluginPitchNamesByNote_` stays empty unless diagnostics (`kDrumNamesDiag`) drive refresh.
} // namespace

namespace experimental_instrument_host_detail
{
struct DrumNameVst3ProbeDetails
{
    bool vst3IComponentPresent = false;
    bool editControllerPresent = false;
    /// "none", "visitor" (JUCE-resolved IUnitInfo), "component", "controller"
    juce::String unitInfoSource;
    int activeProgramIndex = -1;
    int programListCount = 0;
    int selectedListId = -1;
    int selectedProgramIndex = -1;
    juce::String selectedProgramName;
    bool sawProgramListWithPitchNameSupport = false;
    int getProgramPitchNameOkCount = 0;
    juce::String zeroReason;

    // Phase A (filled when verbose diagnostics run)
    int unitCount = -1;
    juce::String selectedUnitIdStr{"n/a"};
    bool keyswitchControllerPresent = false;
    int keyswitchCountBus0Ch0 = -1;
    int keyswitchCountBus0Ch9 = -1;
    bool programListDataOnComponent = false;
    bool programListDataOnController = false;
    bool noteExpressionControllerOnEdit = false;
    int noteExpressionCountBus0Ch0 = -1;
    int noteExpressionCountBus0Ch9 = -1;
    bool midiMappingOnComponent = false;
    bool midiMappingOnController = false;
};

#if JUCE_PLUGINHOST_VST3 && (JUCE_WINDOWS || JUCE_MAC || JUCE_LINUX || JUCE_BSD)
using namespace Steinberg;

[[nodiscard]] int string128RawCodeUnitCountBeforeNul(const Vst::String128& raw)
{
    int len = 0;
    while (len < 128 && raw[len] != 0)
    {
        ++len;
    }
    return len;
}

[[nodiscard]] juce::String string128ToJuce(const Vst::String128& raw)
{
    const int len = string128RawCodeUnitCountBeforeNul(raw);
    if (len <= 0)
    {
        return {};
    }
    return juce::String(
        juce::CharPointer_UTF16(reinterpret_cast<const juce::CharPointer_UTF16::CharType*>(raw)),
        juce::CharPointer_UTF16(reinterpret_cast<const juce::CharPointer_UTF16::CharType*>(raw + len)));
}

/// First `maxUnits` UTF-16 code units as 4-digit hex (for diagnostics; String128 is UTF-16 per VST3).
[[nodiscard]] juce::String string128Utf16HexPreview(const Vst::String128& raw, int maxUnits = 16)
{
    juce::String s;
    const int n = juce::jmin(maxUnits, 128);
    for (int i = 0; i < n; ++i)
    {
        const auto cu = (unsigned int)(char16_t)raw[i];
        if (cu == 0U)
        {
            break;
        }
        if (s.isNotEmpty())
        {
            s << " ";
        }
        s << juce::String::toHexString((int)cu).paddedLeft('0', 4);
    }
    return s;
}

[[nodiscard]] juce::String tresultSymbolicName(tresult rc)
{
    if (rc == static_cast<tresult>(-1))
    {
        return "undocumented_minus_one";
    }
    if (rc == kResultOk)
    {
        return "kResultOk";
    }
    if (rc == kResultFalse)
    {
        return "kResultFalse";
    }
    if (rc == kInvalidArgument)
    {
        return "kInvalidArgument";
    }
    if (rc == kNotImplemented)
    {
        return "kNotImplemented";
    }
    if (rc == kInternalError)
    {
        return "kInternalError";
    }
    if (rc == kNotInitialized)
    {
        return "kNotInitialized";
    }
    if (rc == kOutOfMemory)
    {
        return "kOutOfMemory";
    }
    if (rc == kNoInterface)
    {
        return "kNoInterface";
    }
    return "unknown(rc=" + juce::String((int)rc) + ")";
}

template <typename Iface>
[[nodiscard]] bool vst3QueryPresence(FUnknown* obj)
{
    if (obj == nullptr)
    {
        return false;
    }
    void* ptr = nullptr;
    if (obj->queryInterface(Iface::iid, &ptr) != kResultOk || ptr == nullptr)
    {
        return false;
    }
    static_cast<Iface*>(ptr)->release();
    return true;
}

[[nodiscard]] bool findFirstProgramListForPitchRetest(Vst::IUnitInfo* unitInfo,
                                                      int programListCount,
                                                      Vst::ProgramListID& outListId,
                                                      int32& outProgramIndex)
{
    for (int li = 0; li < programListCount; ++li)
    {
        Vst::ProgramListInfo pli{};
        if (unitInfo->getProgramListInfo(li, pli) != kResultOk)
        {
            continue;
        }
        for (int32 pi = 0; pi < pli.programCount; ++pi)
        {
            if (unitInfo->hasProgramPitchNames(pli.id, pi) == kResultTrue)
            {
                outListId = pli.id;
                outProgramIndex = pi;
                return true;
            }
        }
    }
    if (programListCount <= 0)
    {
        return false;
    }
    Vst::ProgramListInfo pli{};
    if (unitInfo->getProgramListInfo(0, pli) != kResultOk)
    {
        return false;
    }
    outListId = pli.id;
    outProgramIndex = 0;
    return true;
}

static void runDrumNamePhaseAHostInterfaceProbes(Vst::IComponent* component,
                                                 Vst::IEditController* editController,
                                                 DrumNameVst3ProbeDetails& details,
                                                 const std::function<void(const juce::String&)>& logLine)
{
    details.programListDataOnComponent = vst3QueryPresence<Vst::IProgramListData>(component);
    details.programListDataOnController = vst3QueryPresence<Vst::IProgramListData>(editController);
    details.midiMappingOnComponent = vst3QueryPresence<Vst::IMidiMapping>(component);
    details.midiMappingOnController = vst3QueryPresence<Vst::IMidiMapping>(editController);

    if (editController != nullptr)
    {
        Vst::INoteExpressionController* ne = nullptr;
        if (editController->queryInterface(Vst::INoteExpressionController::iid, (void**)&ne) == kResultOk
            && ne != nullptr)
        {
            details.noteExpressionControllerOnEdit = true;
            details.noteExpressionCountBus0Ch0 = (int)ne->getNoteExpressionCount(0, 0);
            details.noteExpressionCountBus0Ch9 = (int)ne->getNoteExpressionCount(0, 9);
            ne->release();
        }
    }

    logLine("drum-names: phaseA IProgramListData component=" + juce::String(details.programListDataOnComponent ? "present" : "absent")
            + " controller=" + juce::String(details.programListDataOnController ? "present" : "absent"));
    logLine("drum-names: phaseA IMidiMapping component=" + juce::String(details.midiMappingOnComponent ? "present" : "absent")
            + " controller=" + juce::String(details.midiMappingOnController ? "present" : "absent"));
    logLine("drum-names: phaseA INoteExpressionController editController="
            + juce::String(details.noteExpressionControllerOnEdit ? "present" : "absent")
            + " noteExprCount bus0 ch0=" + juce::String(details.noteExpressionCountBus0Ch0) + " ch9="
            + juce::String(details.noteExpressionCountBus0Ch9));

    if (editController != nullptr)
    {
        Vst::IKeyswitchController* ksw = nullptr;
        if (editController->queryInterface(Vst::IKeyswitchController::iid, (void**)&ksw) == kResultOk
            && ksw != nullptr)
        {
            details.keyswitchControllerPresent = true;
            details.keyswitchCountBus0Ch0 = (int)ksw->getKeyswitchCount(0, 0);
            details.keyswitchCountBus0Ch9 = (int)ksw->getKeyswitchCount(0, 9);
            logLine("drum-names: phaseA IKeyswitchController present keyswitchCount busIndex=0 ch0="
                    + juce::String(details.keyswitchCountBus0Ch0) + " ch9=" + juce::String(details.keyswitchCountBus0Ch9));
            constexpr int kMaxKeyswitchSamples = 8;
            for (int16 ch : { (int16)0, (int16)9 })
            {
                const int cnt = (int)ksw->getKeyswitchCount(0, ch);
                const int lim = juce::jmin(cnt, kMaxKeyswitchSamples);
                for (int ki = 0; ki < lim; ++ki)
                {
                    Vst::KeyswitchInfo kiInf{};
                    const tresult infRc = ksw->getKeyswitchInfo(0, ch, ki, kiInf);
                    const juce::String title = (infRc == kResultOk) ? string128ToJuce(kiInf.title).trim() : juce::String();
                    logLine("drum-names: phaseA keyswitchSample bus=0 ch=" + juce::String((int)ch) + " idx=" + juce::String(ki)
                            + " getKeyswitchInfo rc=" + juce::String((int)infRc) + "("
                            + tresultSymbolicName(infRc) + ") title=\"" + title + "\" keyswitchMin="
                            + juce::String(kiInf.keyswitchMin) + " keyswitchMax=" + juce::String(kiInf.keyswitchMax));
                }
            }
            ksw->release();
        }
        else
        {
            logLine("drum-names: phaseA IKeyswitchController absent on IEditController");
        }
    }
}

static void runDrumNamePhaseAUnitTreeLog(Vst::IUnitInfo* unitInfo, DrumNameVst3ProbeDetails& details,
                                         const std::function<void(const juce::String&)>& logLine)
{
    details.unitCount = (int)unitInfo->getUnitCount();
    const Vst::UnitID selectedBefore = unitInfo->getSelectedUnit();
    details.selectedUnitIdStr = juce::String((int)selectedBefore);
    logLine("drum-names: phaseA unitTree unitCount=" + juce::String(details.unitCount) + " getSelectedUnit unitId="
            + details.selectedUnitIdStr);
    for (int32 ui = 0; ui < (int32)details.unitCount; ++ui)
    {
        Vst::UnitInfo uinf{};
        if (unitInfo->getUnitInfo(ui, uinf) != kResultOk)
        {
            logLine("drum-names: phaseA unitTree unitIndex=" + juce::String(ui) + " getUnitInfo non_ok");
            continue;
        }
        const juce::String unm = string128ToJuce(uinf.name).trim();
        logLine("drum-names: phaseA unitTree unitIndex=" + juce::String(ui) + " id=" + juce::String((int)uinf.id)
                + " parentUnitId=" + juce::String((int)uinf.parentUnitId)
                + " programListId=" + juce::String((int)uinf.programListId) + " name=\"" + unm + "\"");
    }
}

static void runDrumNamePhaseASelectUnitPitchRetest(Vst::IUnitInfo* unitInfo,
                                                   int programListCount,
                                                   DrumNameVst3ProbeDetails& details,
                                                   const std::function<void(const juce::String&)>& logLine)
{
    const Vst::UnitID selectedBefore = unitInfo->getSelectedUnit();
    Vst::ProgramListID retestListId{};
    int32 retestProgIx = 0;
    const bool haveRetestPair = findFirstProgramListForPitchRetest(unitInfo, programListCount, retestListId, retestProgIx);
    logLine("drum-names: phaseA selectUnitRetest retestListId="
            + juce::String(haveRetestPair ? (int)retestListId : -1) + " retestProgramIndex="
            + juce::String(haveRetestPair ? (int)retestProgIx : -1));

    if (details.unitCount > 0 && haveRetestPair)
    {
        constexpr int kProbeNotes[] = {36, 38, 42, 46};
        for (int32 ui = 0; ui < (int32)details.unitCount; ++ui)
        {
            Vst::UnitInfo uinf{};
            if (unitInfo->getUnitInfo(ui, uinf) != kResultOk)
            {
                continue;
            }
            const tresult sru = unitInfo->selectUnit(uinf.id);
            logLine("drum-names: phaseA selectUnitRetest selectUnit(unitId=" + juce::String((int)uinf.id) + ") rc="
                    + juce::String((int)sru) + "(" + tresultSymbolicName(sru) + ")");
            for (int note : kProbeNotes)
            {
                Vst::String128 nm{};
                const tresult prc = unitInfo->getProgramPitchName(retestListId, retestProgIx, (int16)note, nm);
                logLine("drum-names: phaseA selectUnitRetest afterSelect unitId=" + juce::String((int)uinf.id) + " note="
                        + juce::String(note) + " getProgramPitchName rc=" + juce::String((int)prc) + "("
                        + tresultSymbolicName(prc) + ") rawCodeUnits=" + juce::String(string128RawCodeUnitCountBeforeNul(nm)));
            }
        }
        const tresult restoreRc = unitInfo->selectUnit(selectedBefore);
        logLine("drum-names: phaseA selectUnitRetest restore selectUnit(originalUnitId="
                + juce::String((int)selectedBefore) + ") rc=" + juce::String((int)restoreRc) + "("
                + tresultSymbolicName(restoreRc) + ")");
    }
    else
    {
        logLine("drum-names: phaseA selectUnitRetest skipped (no units or no program list for pitch retest)");
    }
}

static void runDrumNameProgramListVerboseScan(Vst::IUnitInfo* unitInfo,
                                              int programListCount,
                                              const std::function<void(const juce::String&)>& logLine)
{
    static_assert(sizeof(char16_t) == 2, "VST3 String128 uses char16_t UTF-16 code units");
    logLine(
        "drum-names: String128 note: char16_t code units UTF-16; we build juce::String from units before first "
        "0x0000 via CharPointer_UTF16 (if kResultOk but conv empty while rawCodeUnits>0, treat as conversion issue)");

    for (int li = 0; li < programListCount; ++li)
    {
        Vst::ProgramListInfo pli{};
        if (unitInfo->getProgramListInfo(li, pli) != kResultOk)
        {
            logLine("drum-names: programList[listIndex=" + juce::String(li) + "] getProgramListInfo non_ok");
            continue;
        }

        const juce::String listName = string128ToJuce(pli.name).trim();
        logLine("drum-names: programList listIndex=" + juce::String(li) + " listId=" + juce::String((int)pli.id)
                + " listName=\"" + listName + "\" programCount=" + juce::String((int)pli.programCount));

        for (int pi = 0; pi < pli.programCount; ++pi)
        {
            Vst::String128 progNmRaw{};
            const tresult nameRc = unitInfo->getProgramName(pli.id, pi, progNmRaw);
            juce::String programName;
            if (nameRc == kResultOk)
            {
                programName = string128ToJuce(progNmRaw).trim();
            }
            else
            {
                programName = "(getProgramName non_ok rc=" + juce::String((int)nameRc) + ")";
            }

            const tresult hasPitch = unitInfo->hasProgramPitchNames(pli.id, pi);
            juce::String hasPitchS = "unknown";
            if (hasPitch == kResultTrue)
            {
                hasPitchS = "true";
            }
            else if (hasPitch == kResultFalse)
            {
                hasPitchS = "false";
            }

            int cNonOk = 0;
            int cOkApiEmptyBuffer = 0;
            int cOkNonEmptyAfterTrim = 0;
            int cOkRawHasTrimEmpty = 0;
            int cOkRawHasConvEmpty = 0;
            for (int pitch = 0; pitch <= 127; ++pitch)
            {
                Vst::String128 nm{};
                const tresult prc = unitInfo->getProgramPitchName(pli.id, pi, (int16)pitch, nm);
                if (prc != kResultOk)
                {
                    ++cNonOk;
                    continue;
                }
                const int rawLen = string128RawCodeUnitCountBeforeNul(nm);
                if (rawLen == 0)
                {
                    ++cOkApiEmptyBuffer;
                    continue;
                }
                const juce::String conv = string128ToJuce(nm);
                const juce::String trimmed = conv.trim();
                if (trimmed.isNotEmpty())
                {
                    ++cOkNonEmptyAfterTrim;
                    continue;
                }
                ++cOkRawHasTrimEmpty;
                if (conv.isEmpty())
                {
                    ++cOkRawHasConvEmpty;
                }
            }

            logLine("drum-names:  program listId=" + juce::String((int)pli.id) + " programIndex=" + juce::String(pi)
                    + " programName=\"" + programName + "\" hasProgramPitchNames=" + hasPitchS
                    + " getProgramPitchName(0-127) nonOk=" + juce::String(cNonOk)
                    + " okApiEmptyBuffer=" + juce::String(cOkApiEmptyBuffer)
                    + " okNonEmptyAfterTrim=" + juce::String(cOkNonEmptyAfterTrim)
                    + " okRawHadCharsTrimEmpty=" + juce::String(cOkRawHasTrimEmpty)
                    + " ofWhich_okButJuceConvEmptyWhileRawHadUnits=" + juce::String(cOkRawHasConvEmpty));

            constexpr int kSampleNotes[] = {36, 38, 42, 46};
            for (int note : kSampleNotes)
            {
                Vst::String128 nm{};
                const tresult prc = unitInfo->getProgramPitchName(pli.id, pi, (int16)note, nm);
                const int rawLen = string128RawCodeUnitCountBeforeNul(nm);
                const juce::String rawHex = string128Utf16HexPreview(nm, 16);
                const juce::String conv = (prc == kResultOk) ? string128ToJuce(nm) : juce::String();
                const juce::String trimmed = conv.trim();
                juce::String cls;
                if (prc != kResultOk)
                {
                    cls = "non_ok";
                }
                else if (rawLen == 0)
                {
                    cls = "ok_apiEmptyBuffer";
                }
                else if (trimmed.isNotEmpty())
                {
                    cls = "ok_nonEmpty";
                }
                else if (conv.isEmpty())
                {
                    cls = "ok_rawHadUnits_juceConvEmpty";
                }
                else
                {
                    cls = "ok_whitespaceOnly";
                }

                logLine("drum-names:  pitchSample listId=" + juce::String((int)pli.id)
                        + " programIndex=" + juce::String(pi) + " note=" + juce::String(note)
                        + " rc=" + juce::String((int)prc) + "(" + tresultSymbolicName(prc) + ")"
                        + " rawCodeUnits=" + juce::String(rawLen)
                        + " rawUtf16Hex=\"" + rawHex + "\" convNoTrim=\"" + conv + "\" convTrimmed=\"" + trimmed
                        + "\" class=" + cls);
            }
        }
    }
}

struct Vst3DrumNameInterfacesCapture final : juce::ExtensionsVisitor
{
    Vst::IComponent* component = nullptr;
    Vst::IEditController* editController = nullptr;
    Vst::IUnitInfo* unitInfoFromVisitor = nullptr;

    void visitVST3Client(const VST3Client& c) override
    {
        component = c.getIComponentPtr();
        editController = c.getIEditControllerPtr();
        unitInfoFromVisitor = c.getIUnitInfoPtr();
    }
};

struct ScopedVst3UnitInfoQuery
{
    Vst::IUnitInfo* p = nullptr;
    bool ownsAddRef = false;
    ~ScopedVst3UnitInfoQuery()
    {
        if (ownsAddRef && p != nullptr)
        {
            p->release();
        }
    }
};

/// Returns number of MIDI notes (0-127) with a non-empty pitch name. `outMap` is replaced.
[[nodiscard]] int tryFillPitchNamesFromVst3UnitInfo(
    juce::AudioPluginInstance& inst,
    std::map<int, juce::String>& outMap,
    DrumNameVst3ProbeDetails& details,
    const std::function<void(const juce::String&)>* verboseLog /* nullable; message thread */)
{
    outMap.clear();
    details = {};
    details.unitInfoSource = "none";

    Vst3DrumNameInterfacesCapture cap;
    inst.getExtensions(cap);
    details.vst3IComponentPresent = cap.component != nullptr;
    details.editControllerPresent = cap.editController != nullptr;
    if (cap.component == nullptr)
    {
        details.zeroReason = "VST3 IComponent missing (getExtensions did not provide a VST3 client pointer)";
        return 0;
    }

    ScopedVst3UnitInfoQuery unitInfoHolder;
    if (cap.unitInfoFromVisitor != nullptr)
    {
        unitInfoHolder.p = cap.unitInfoFromVisitor;
        unitInfoHolder.ownsAddRef = false;
        details.unitInfoSource = "visitor";
    }
    else
    {
        if (cap.component->queryInterface(Vst::IUnitInfo::iid, (void**)&unitInfoHolder.p) == kResultOk
            && unitInfoHolder.p != nullptr)
        {
            unitInfoHolder.ownsAddRef = true;
            details.unitInfoSource = "component";
        }
        else if (cap.editController != nullptr
                 && cap.editController->queryInterface(Vst::IUnitInfo::iid, (void**)&unitInfoHolder.p) == kResultOk
                 && unitInfoHolder.p != nullptr)
        {
            unitInfoHolder.ownsAddRef = true;
            details.unitInfoSource = "controller";
        }
    }

    Vst::IUnitInfo* const unitInfo = unitInfoHolder.p;
    if (unitInfo == nullptr)
    {
        details.zeroReason = "IUnitInfo unavailable: JUCE visitor had no IUnitInfo and queryInterface failed on "
                             "IComponent and IEditController (typical for split component/controller if not resolved)";
        return 0;
    }

    if (cap.editController != nullptr)
    {
        const int n = cap.editController->getParameterCount();
        for (int idx = 0; idx < n; ++idx)
        {
            Vst::ParameterInfo paramInfo{};
            if (cap.editController->getParameterInfo(idx, paramInfo) != kResultOk)
            {
                continue;
            }
            if ((paramInfo.flags & Vst::ParameterInfo::kIsProgramChange) == 0)
            {
                continue;
            }
            const int sc = (int)paramInfo.stepCount;
            if (sc <= 0)
            {
                details.activeProgramIndex = 0;
            }
            else
            {
                const double norm = (double)cap.editController->getParamNormalized(paramInfo.id);
                int pi = (int)std::lround(norm * (double)sc);
                pi = juce::jlimit(0, sc, pi);
                details.activeProgramIndex = pi;
            }
            break;
        }
    }

    details.programListCount = (int)unitInfo->getProgramListCount();

    if (verboseLog != nullptr)
    {
        runDrumNamePhaseAHostInterfaceProbes(cap.component, cap.editController, details, *verboseLog);
        runDrumNamePhaseAUnitTreeLog(unitInfo, details, *verboseLog);
    }

    if (details.programListCount <= 0)
    {
        details.zeroReason = "IUnitInfo reports no program lists";
        return 0;
    }

    if (verboseLog != nullptr)
    {
        runDrumNameProgramListVerboseScan(unitInfo, details.programListCount, *verboseLog);
        runDrumNamePhaseASelectUnitPitchRetest(unitInfo, details.programListCount, details, *verboseLog);
    }
    for (int li = 0; li < details.programListCount; ++li)
    {
        Vst::ProgramListInfo pli{};
        if (unitInfo->getProgramListInfo(li, pli) != kResultOk)
        {
            continue;
        }

        juce::Array<int> piOrder;
        const int pref = details.activeProgramIndex;
        if (pref >= 0 && pref < pli.programCount)
        {
            piOrder.add(pref);
        }
        for (int pi = 0; pi < pli.programCount; ++pi)
        {
            if (pi != pref)
            {
                piOrder.add(pi);
            }
        }

        for (int opi = 0; opi < piOrder.size(); ++opi)
        {
            const int pi = piOrder[opi];
            if (unitInfo->hasProgramPitchNames(pli.id, pi) != kResultTrue)
            {
                continue;
            }

            details.sawProgramListWithPitchNameSupport = true;
            details.selectedListId = (int)pli.id;
            details.selectedProgramIndex = pi;
            details.selectedProgramName.clear();
            Vst::String128 progNmRaw{};
            if (unitInfo->getProgramName(pli.id, pi, progNmRaw) == kResultOk)
            {
                details.selectedProgramName = string128ToJuce(progNmRaw).trim();
            }
            details.getProgramPitchNameOkCount = 0;

            std::map<int, juce::String> chunk;
            for (int pitch = 0; pitch <= 127; ++pitch)
            {
                Vst::String128 nm{};
                if (unitInfo->getProgramPitchName(pli.id, pi, (int16)pitch, nm) != kResultOk)
                {
                    continue;
                }
                ++details.getProgramPitchNameOkCount;
                juce::String s = string128ToJuce(nm).trim();
                if (s.isEmpty())
                {
                    continue;
                }
                chunk[pitch] = std::move(s);
            }

            if (!chunk.empty())
            {
                outMap = std::move(chunk);
                details.zeroReason.clear();
                return (int)outMap.size();
            }
        }
    }

    if (!details.sawProgramListWithPitchNameSupport)
    {
        details.zeroReason = "no program/list index has hasProgramPitchNames; kit may not expose VST3 pitch names";
    }
    else
    {
        details.zeroReason = "hasProgramPitchNames was true for at least one program but getProgramPitchName returned "
                             "no non-empty names for MIDI 0-127 (all empty or non-ok)";
    }
    return 0;
}

void runPhaseCMetadataDiagnostics(
    juce::AudioPluginInstance& inst,
    const std::map<int, juce::String>& rawMap,
    std::set<int>& outCandidateNotes,
    juce::String& outReason,
    const std::function<void(const juce::String&)>& logLine)
{
    using Vst::IUnitInfo;
    outCandidateNotes.clear();
    outReason.clear();
    std::set<int> keyswitchGrooveExclude;

    auto countBuckets = [](IUnitInfo* ui, Vst::ProgramListID lid, const int32 pi, int& o0, int& o24, int& o36,
                           int& o52) {
        o0 = o24 = o36 = o52 = 0;
        for (int pitch = 0; pitch <= 127; ++pitch)
        {
            Vst::String128 nm{};
            if (ui->getProgramPitchName(lid, pi, (int16)pitch, nm) != kResultOk)
            {
                continue;
            }
            if (string128ToJuce(nm).trim().isEmpty())
            {
                continue;
            }
            if (pitch <= 23)
            {
                ++o0;
            }
            else if (pitch <= 35)
            {
                ++o24;
            }
            else if (pitch <= 51)
            {
                ++o36;
            }
            else
            {
                ++o52;
            }
        }
    };

    Vst3DrumNameInterfacesCapture cap;
    inst.getExtensions(cap);
    if (cap.component == nullptr)
    {
        logLine("drum-names: phaseC metadata aborted reason=noIComponent");
        outReason = "noIComponent";
        return;
    }

    ScopedVst3UnitInfoQuery holder;
    if (cap.unitInfoFromVisitor != nullptr)
    {
        holder.p = cap.unitInfoFromVisitor;
        holder.ownsAddRef = false;
    }
    else if (cap.component->queryInterface(IUnitInfo::iid, (void**)&holder.p) == kResultOk && holder.p != nullptr)
    {
        holder.ownsAddRef = true;
    }
    else if (cap.editController != nullptr
             && cap.editController->queryInterface(IUnitInfo::iid, (void**)&holder.p) == kResultOk
             && holder.p != nullptr)
    {
        holder.ownsAddRef = true;
    }

    IUnitInfo* const ui = holder.p;
    if (ui == nullptr)
    {
        logLine("drum-names: phaseC metadata aborted reason=noIUnitInfo");
        outReason = "noIUnitInfo";
        return;
    }

    const int plc = (int)ui->getProgramListCount();
    logLine("drum-names: phaseC multiList programListCount=" + juce::String(plc));

    for (int li = 0; li < plc; ++li)
    {
        Vst::ProgramListInfo pli{};
        if (ui->getProgramListInfo(li, pli) != kResultOk)
        {
            continue;
        }
        const juce::String listName = string128ToJuce(pli.name).trim();
        for (int32 pi = 0; pi < pli.programCount; ++pi)
        {
            const tresult hp = ui->hasProgramPitchNames(pli.id, pi);
            const juce::String hpS
                = (hp == kResultTrue) ? "true" : ((hp == kResultFalse) ? "false" : "unknown");
            if (hp != kResultTrue)
            {
                logLine("drum-names: phaseC multiList listIndex=" + juce::String(li) + " listId="
                        + juce::String((int)pli.id) + " listName=\"" + listName + "\" programIndex="
                        + juce::String((int)pi) + " hasProgramPitchNames=" + hpS + " buckets=skipped");
                continue;
            }
            int b0 = 0, b24 = 0, b36 = 0, b52 = 0;
            countBuckets(ui, pli.id, pi, b0, b24, b36, b52);
            juce::String progNm;
            Vst::String128 progNmRaw{};
            if (ui->getProgramName(pli.id, pi, progNmRaw) == kResultOk)
            {
                progNm = string128ToJuce(progNmRaw).trim();
            }
            logLine("drum-names: phaseC multiList listIndex=" + juce::String(li) + " listId="
                    + juce::String((int)pli.id) + " listName=\"" + listName + "\" programIndex="
                    + juce::String((int)pi) + " programName=\"" + progNm + "\" hasProgramPitchNames=" + hpS
                    + " bucket0_23=" + juce::String(b0) + " bucket24_35=" + juce::String(b24)
                    + " bucket36_51=" + juce::String(b36) + " bucket52_127=" + juce::String(b52));

            static const char* attrKeys[] = {"Instrument", "Style",     "Character",
                                             "Name",       "FilePath", "FilePathStringType"};
            for (const char* ak : attrKeys)
            {
                Vst::String128 aval{};
                if (ui->getProgramInfo(pli.id, pi, ak, aval) != kResultOk)
                {
                    continue;
                }
                const juce::String av = string128ToJuce(aval).trim();
                if (av.isEmpty())
                {
                    continue;
                }
                logLine("drum-names: phaseC programAttr listId=" + juce::String((int)pli.id) + " programIndex="
                        + juce::String((int)pi) + " key=\"" + juce::String(ak) + "\" value=\"" + av + "\"");
            }
        }
    }

    for (int ch = 0; ch < 16; ++ch)
    {
        Vst::UnitID uid = (Vst::UnitID)-1;
        const tresult tr = ui->getUnitByBus((Vst::MediaType)1, (Vst::BusDirection)0, 0, ch, uid);
        logLine("drum-names: phaseC unitByBus type=kEvent dir=kInput busIndex=0 channel=" + juce::String(ch)
                + " rc=" + juce::String((int)tr) + "(" + tresultSymbolicName(tr) + ") unitId="
                + juce::String((int)uid));
    }

    if (cap.editController != nullptr)
    {
        Vst::IKeyswitchController* ksw = nullptr;
        if (cap.editController->queryInterface(Vst::IKeyswitchController::iid, (void**)&ksw) == kResultOk
            && ksw != nullptr)
        {
            for (int16 ch : { (int16)0, (int16)9 })
            {
                const int cnt = (int)ksw->getKeyswitchCount(0, ch);
                for (int ki = 0; ki < cnt; ++ki)
                {
                    Vst::KeyswitchInfo kiInf{};
                    const tresult ir = ksw->getKeyswitchInfo(0, ch, ki, kiInf);
                    const juce::String title
                        = (ir == kResultOk) ? string128ToJuce(kiInf.title).trim() : juce::String{};
                    logLine("drum-names: phaseC keyswitch bus=0 ch=" + juce::String((int)ch) + " idx="
                            + juce::String(ki) + " rc=" + juce::String((int)ir) + " title=\"" + title + "\" min="
                            + juce::String((int)kiInf.keyswitchMin) + " max=" + juce::String((int)kiInf.keyswitchMax));
                    if (title.containsIgnoreCase("groove"))
                    {
                        const int mn = (int)kiInf.keyswitchMin;
                        const int mx = (int)kiInf.keyswitchMax;
                        for (int n = mn; n <= mx; ++n)
                        {
                            if (n >= 0 && n <= 127)
                            {
                                keyswitchGrooveExclude.insert(n);
                            }
                        }
                    }
                }
            }
            ksw->release();
        }
    }

    std::map<juce::String, std::vector<int>> byName;
    for (const auto& kv : rawMap)
    {
        byName[kv.second].push_back(kv.first);
    }
    for (auto& pr : byName)
    {
        auto& notes = pr.second;
        if (notes.size() < 2)
        {
            continue;
        }
        std::sort(notes.begin(), notes.end());
        juce::String line = "drum-names: phaseC duplicateRawName name=\"" + pr.first + "\" notes=";
        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (i > 0)
            {
                line << ",";
            }
            line << notes[i];
        }
        logLine(line);
    }

    std::set<int> stage1;
    for (auto& pr : byName)
    {
        const juce::String& name = pr.first;
        auto notes = pr.second;
        std::sort(notes.begin(), notes.end());
        bool anyInPad = false;
        for (int n : notes)
        {
            if (n >= 36 && n <= 51)
            {
                anyInPad = true;
                break;
            }
        }
        if (anyInPad)
        {
            for (int n : notes)
            {
                if (n >= 36 && n <= 51)
                {
                    stage1.insert(n);
                }
            }
        }
        else
        {
            if (name.toLowerCase().startsWith("groove"))
            {
                continue;
            }
            for (int n : notes)
            {
                stage1.insert(n);
            }
        }
    }

    for (int n : keyswitchGrooveExclude)
    {
        stage1.erase(n);
    }

    outCandidateNotes = std::move(stage1);
    outReason = "heuristic=name_duplicate_prefers_36_51;groove_prefix_excluded;keyswitch_groove_title_notes_excluded="
                + juce::String((int)keyswitchGrooveExclude.size());
}
#else
[[nodiscard]] int tryFillPitchNamesFromVst3UnitInfo(
    juce::AudioPluginInstance&,
    std::map<int, juce::String>& outMap,
    DrumNameVst3ProbeDetails& details,
    const std::function<void(const juce::String&)>* /*verboseLog*/)
{
    outMap.clear();
    details = {};
    details.zeroReason = "VST3 host disabled or unsupported platform build";
    return 0;
}
#endif
} // namespace experimental_instrument_host_detail

namespace
{
    [[nodiscard]] int countKeyPadNotesWithNonEmptyNames(const std::map<int, juce::String>& m) noexcept
    {
        static constexpr int kNotes[] = {36, 38, 42, 46};
        int c = 0;
        for (const int note : kNotes)
        {
            const auto it = m.find(note);
            if (it != m.end() && it->second.isNotEmpty())
            {
                ++c;
            }
        }
        return c;
    }

    struct PluginDrumMapAuthorityEval
    {
        bool authoritative = false;
        juce::String reason;
    };

    [[nodiscard]] PluginDrumMapAuthorityEval evaluatePluginDrumMapAuthority(
        const drum_name_diag::DrumNameRefreshPhase phase,
        const int namesFound,
        const std::map<int, juce::String>& m)
    {
        PluginDrumMapAuthorityEval r;
        if (namesFound <= 0 || m.empty())
        {
            r.reason = "empty_map";
            return r;
        }

        const int keyPads = countKeyPadNotesWithNonEmptyNames(m);
        const bool afterEditorOpen = (phase == drum_name_diag::DrumNameRefreshPhase::afterEditorOpen);

        if (afterEditorOpen)
        {
            if (namesFound >= 16)
            {
                r.authoritative = true;
                r.reason = "afterEditorOpen_namesFound>=16";
                return r;
            }
            if (namesFound >= 8 && keyPads >= 2)
            {
                r.authoritative = true;
                r.reason = "afterEditorOpen_namesFound>=8_keyPadsNonEmpty>=2";
                return r;
            }
            if (namesFound >= 6 && keyPads >= 3)
            {
                r.authoritative = true;
                r.reason = "afterEditorOpen_namesFound>=6_keyPadsNonEmpty>=3";
                return r;
            }
            r.reason = "afterEditorOpen_insufficient namesFound=" + juce::String(namesFound)
                       + " keyPadsNonEmpty=" + juce::String(keyPads);
            return r;
        }

        r.authoritative = false;
        r.reason = "phase_not_afterEditorOpen namesFound=" + juce::String(namesFound)
                   + " keyPadsNonEmpty=" + juce::String(keyPads)
                   + " phase=" + juce::String(drum_name_diag::drumNameRefreshPhaseTag(phase));
        return r;
    }

    [[nodiscard]] drum_name_diag::PluginDrumNameMapTrust trustLevelForMap(
        const bool authoritative,
        const std::map<int, juce::String>& m) noexcept
    {
        if (m.empty())
        {
            return drum_name_diag::PluginDrumNameMapTrust::none;
        }
        if (authoritative)
        {
            return drum_name_diag::PluginDrumNameMapTrust::authoritative;
        }
        return drum_name_diag::PluginDrumNameMapTrust::candidate;
    }

    [[nodiscard]] bool isGroovePatternTriggerPitchName(const juce::String& rawName) noexcept
    {
        return rawName.trim().startsWithIgnoreCase("Groove ");
    }

    struct DerivePrimaryPadDisplayResult
    {
        std::set<int> notes;
        juce::String reason;
        bool ok = false;
    };

    /// Infers visible primary drum pads from duplicate pitch-name structure (secondary low zone vs higher pads).
    /// Does not return a hardcoded 36–51 constant; Groove-style maps satisfy structural confidence checks.
    [[nodiscard]] DerivePrimaryPadDisplayResult derivePrimaryDrumPadDisplayNotesFromRawMap(
        const std::map<int, juce::String>& raw)
    {
        DerivePrimaryPadDisplayResult r;
        std::map<juce::String, std::vector<int>> byName;
        for (const auto& kv : raw)
        {
            if (kv.second.isEmpty() || isGroovePatternTriggerPitchName(kv.second))
            {
                continue;
            }
            byName[kv.second].push_back(kv.first);
        }

        std::vector<int> anchors;
        int wideDuplicateGroups = 0;
        for (auto& pr : byName)
        {
            auto& notes = pr.second;
            if (notes.size() < 2)
            {
                continue;
            }
            std::sort(notes.begin(), notes.end());
            if (notes.back() - notes.front() >= 6)
            {
                ++wideDuplicateGroups;
            }
            anchors.push_back(notes.back());
        }

        if ((int)anchors.size() < 8)
        {
            r.reason = "insufficient_duplicate_groups";
            return r;
        }
        if (wideDuplicateGroups < 4)
        {
            r.reason = "insufficient_wide_duplicate_separation";
            return r;
        }

        const int lo = *std::min_element(anchors.begin(), anchors.end());
        const int hi = *std::max_element(anchors.begin(), anchors.end());
        if (hi - lo < 7)
        {
            r.reason = "cluster_span_too_small";
            return r;
        }
        if (lo < 24)
        {
            r.reason = "cluster_starts_too_low";
            return r;
        }

        std::set<int> cluster;
        for (const auto& kv : raw)
        {
            const int n = kv.first;
            if (n < lo || n > hi || kv.second.isEmpty() || isGroovePatternTriggerPitchName(kv.second))
            {
                continue;
            }
            cluster.insert(n);
        }

        if ((int)cluster.size() < 8)
        {
            r.reason = "insufficient_cluster_named_notes";
            return r;
        }

        juce::String blob;
        for (const int n : cluster)
        {
            const auto it = raw.find(n);
            if (it != raw.end())
            {
                blob << it->second.toLowerCase();
            }
        }
        int categoryHits = 0;
        if (blob.contains("kick"))
        {
            ++categoryHits;
        }
        if (blob.contains("snare"))
        {
            ++categoryHits;
        }
        if (blob.contains("hihat") || blob.contains("hi hat") || blob.contains("hi-hat"))
        {
            ++categoryHits;
        }
        if (blob.contains("crash"))
        {
            ++categoryHits;
        }
        if (blob.contains("ride"))
        {
            ++categoryHits;
        }
        if (blob.contains("tom"))
        {
            ++categoryHits;
        }
        if (categoryHits < 3)
        {
            r.reason = "missing_drum_name_token_evidence";
            return r;
        }

        r.notes = std::move(cluster);
        r.ok = true;
        r.reason = "duplicate_names_prefer_higher_primary_cluster";
        return r;
    }

    void buildPrimaryPadDisplayMapFromNoteSet(const std::map<int, juce::String>& raw,
                                              std::map<int, juce::String>& outDisplay,
                                              const std::set<int>& noteSet)
    {
        outDisplay.clear();
        for (const int n : noteSet)
        {
            const auto it = raw.find(n);
            if (it != raw.end() && it->second.isNotEmpty())
            {
                outDisplay[n] = it->second;
            }
        }
    }

    void buildPrimaryPadDisplayMap(const std::map<int, juce::String>& raw,
                                   std::map<int, juce::String>& outDisplay,
                                   const int padMin,
                                   const int padMax)
    {
        outDisplay.clear();
        for (const auto& kv : raw)
        {
            if (kv.first >= padMin && kv.first <= padMax && kv.second.isNotEmpty())
            {
                outDisplay[kv.first] = kv.second;
            }
        }
    }

    [[nodiscard]] std::set<int> fallbackPrimaryPadNoteSetFromRaw(const std::map<int, juce::String>& raw,
                                                               const int padMin,
                                                               const int padMax)
    {
        std::set<int> s;
        for (int n = padMin; n <= padMax; ++n)
        {
            const auto it = raw.find(n);
            if (it != raw.end() && it->second.isNotEmpty())
            {
                s.insert(n);
            }
        }
        return s;
    }

    [[nodiscard]] juce::String formatIntSetForDiagLog(const std::set<int>& s)
    {
        juce::String r = "{";
        bool first = true;
        for (const int n : s)
        {
            if (!first)
            {
                r << ",";
            }
            first = false;
            r << n;
        }
        r << "}";
        return r;
    }

    // `experimental-instrument.log`: only created/appended when MINIDAW_DIAG_INSTRUMENT_LIFECYCLE != 0 at
    // compile time (`DiagnosticBuildFlags.h`, default **`0`**). Instrument-lifecycle diagnostics only — not contract.

#if MINIDAW_DIAG_INSTRUMENT_LIFECYCLE
    [[nodiscard]] juce::File getExperimentalInstrumentLogFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab")
            .getChildFile("experimental-instrument.log");
    }
#endif

    [[nodiscard]] juce::File getExperimentalVst3ScanDiagnosticLogFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab")
            .getChildFile("experimental-vst3-scan-diagnostic.log");
    }

    void initExperimentalInstrumentSessionLog()
    {
#if !MINIDAW_DIAG_INSTRUMENT_LIFECYCLE
        return;
#else
        try
        {
            const juce::File f = getExperimentalInstrumentLogFile();
            if (!f.getParentDirectory().createDirectory())
            {
                if (!f.getParentDirectory().isDirectory())
                {
                    return;
                }
            }
            const juce::String ts = juce::Time::getCurrentTime().toISO8601(true);
            const juce::String header
                = ts + " === experimental instrument host session start ===\n";
            (void)f.replaceWithText(header, false, false);
        }
        catch (...)
        {
        }
#endif
    }

    void writeExperimentalInstrumentLogLine(const juce::String& message)
    {
#if !MINIDAW_DIAG_INSTRUMENT_LIFECYCLE
        (void)message;
#else
        try
        {
            const juce::File f = getExperimentalInstrumentLogFile();
            if (!f.getParentDirectory().isDirectory())
            {
                (void)f.getParentDirectory().createDirectory();
            }
            const juce::String ts = juce::Time::getCurrentTime().toISO8601(true);
            const juce::String line = ts + " " + message + "\n";
            (void)f.appendText(line);
        }
        catch (...)
        {
        }
#endif
    }

    /// Scan / load boundary lines for `experimental-instrument.log` (compile-gated lifecycle flag above).
    void writeExperimentalInstrumentScanBoundaryLine(const juce::String& message)
    {
#if !MINIDAW_DIAG_INSTRUMENT_LIFECYCLE
        (void)message;
#else
        try
        {
            const juce::File f = getExperimentalInstrumentLogFile();
            if (!f.getParentDirectory().isDirectory())
            {
                (void)f.getParentDirectory().createDirectory();
            }
            const juce::String line = juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n";
            const char* const utf8 = line.toRawUTF8();
            const size_t numBytes = (size_t)line.getNumBytesAsUTF8();
            if (utf8 != nullptr && numBytes > 0U)
            {
                if (f.appendData(utf8, numBytes))
                {
                    return;
                }
            }
            (void)f.appendText(line);
        }
        catch (...)
        {
        }
#endif
    }

    void writeScanDiagnosticScanBoundaryLine(const juce::String& message)
    {
        try
        {
            const juce::File f = getExperimentalVst3ScanDiagnosticLogFile();
            if (!f.getParentDirectory().isDirectory())
            {
                (void)f.getParentDirectory().createDirectory();
            }
            const juce::String line = juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n";
            const char* const utf8 = line.toRawUTF8();
            const size_t numBytes = (size_t)line.getNumBytesAsUTF8();
            if (utf8 != nullptr && numBytes > 0U)
            {
                if (f.appendData(utf8, numBytes))
                {
                    return;
                }
            }
            (void)f.appendText(line);
        }
        catch (...)
        {
        }
    }

    [[nodiscard]] juce::String formatInstrumentHostThreadLine(const juce::String& prefix)
    {
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        const bool isMsgThread = mm != nullptr && mm->isThisTheMessageThread();
        return prefix + " threadId=0x"
             + juce::String::toHexString((juce::int64)(juce::pointer_sized_int)juce::Thread::getCurrentThreadId())
             + " MessageManager::isThisTheMessageThread()="
             + juce::String(isMsgThread ? "true" : "false");
    }

    /// Shared `findAllTypesForFile` loop + flushed BEFORE/AFTER boundary lines.
    template <typename WriteBoundary>
    void runVst3FindAllTypesForFileLoop(const juce::String& pathOrId,
                                        juce::AudioPluginFormatManager& fm,
                                        juce::OwnedArray<juce::PluginDescription>& list,
                                        WriteBoundary&& writeBoundary)
    {
        writeBoundary(formatInstrumentHostThreadLine("scan: boundary before findAllTypesForFile (enter scan) path=\""
                                                     + pathOrId + "\""));

        bool findAllTypesWasInvoked = false;
        for (int i = 0; i < fm.getNumFormats(); ++i)
        {
            juce::AudioPluginFormat* const fmt = fm.getFormat(i);
            if (fmt == nullptr || !fmt->fileMightContainThisPluginType(pathOrId))
            {
                continue;
            }

            const juce::String fmtName = fmt->getName();
            writeBoundary("findAllTypesForFile: BEFORE format=\"" + fmtName + "\" path=\"" + pathOrId
                          + "\" accumulatedCount=" + juce::String(list.size()));

            findAllTypesWasInvoked = true;
            fmt->findAllTypesForFile(list, pathOrId);

            writeBoundary("findAllTypesForFile: AFTER format=\"" + fmtName + "\" path=\"" + pathOrId
                          + "\" accumulatedCount=" + juce::String(list.size()));
        }

        if (list.isEmpty())
        {
            writeBoundary("findAllTypesForFile: scan finished with ZERO descriptions path=\"" + pathOrId
                          + "\" findAllTypesWasInvoked=" + juce::String(findAllTypesWasInvoked ? "true" : "false"));
        }
        else
        {
            writeBoundary("findAllTypesForFile: scan finished with non-zero descriptions count="
                          + juce::String(list.size()) + " path=\"" + pathOrId + "\"");
        }
    }

    [[nodiscard]] juce::String summarizeProcessorBuses(const juce::AudioProcessor& proc)
    {
        const juce::AudioProcessor::BusesLayout lay = proc.getBusesLayout();
        juce::String s = "buses inCount=" + juce::String(lay.inputBuses.size())
                         + " outCount=" + juce::String(lay.outputBuses.size());
        if (!lay.inputBuses.isEmpty())
        {
            s << " inCh[0]=" << lay.inputBuses.getReference(0).size();
        }
        if (!lay.outputBuses.isEmpty())
        {
            s << " outCh[0]=" << lay.outputBuses.getReference(0).size();
        }
        s << " totalIn=" << proc.getTotalNumInputChannels() << " totalOut=" << proc.getTotalNumOutputChannels()
          << " mainIn=" << proc.getMainBusNumInputChannels() << " mainOut=" << proc.getMainBusNumOutputChannels();
        return s;
    }

    [[nodiscard]] juce::PluginDescription scanVst3FileAndLogDescriptions(const juce::File& vst3File,
                                                                         juce::AudioPluginFormatManager& fm,
                                                                         juce::String& err)
    {
        try
        {
            err.clear();
            const juce::String pathOrId = vst3File.getFullPathName();
            juce::OwnedArray<juce::PluginDescription> list;

            runVst3FindAllTypesForFileLoop(
                pathOrId,
                fm,
                list,
                [](const juce::String& msg) { writeExperimentalInstrumentScanBoundaryLine(msg); });

            if (list.isEmpty())
            {
                err = "VST3 scan found no plugin types for this file.";
                return {};
            }

            writeExperimentalInstrumentLogLine(
                "findAllTypesForFile: path=\"" + pathOrId + "\" count=" + juce::String(list.size()));
            for (int i = 0; i < list.size(); ++i)
            {
                const juce::PluginDescription* d = list[i];
                if (d == nullptr)
                {
                    writeExperimentalInstrumentLogLine(
                        "  [" + juce::String(i) + "] (null description entry)");
                    continue;
                }
                juce::String line = "  [" + juce::String(i) + "] name=\"" + d->name + "\"";
                if (d->descriptiveName.isNotEmpty() && d->descriptiveName != d->name)
                {
                    line << " descriptiveName=\"" << d->descriptiveName << "\"";
                }
                if (d->category.isNotEmpty())
                {
                    line << " category=\"" << d->category << "\"";
                }
                if (d->manufacturerName.isNotEmpty())
                {
                    line << " manufacturer=\"" << d->manufacturerName << "\"";
                }
                writeExperimentalInstrumentLogLine(line);
            }

            return *list.getFirst();
        }
        catch (const std::exception& e)
        {
            writeExperimentalInstrumentScanBoundaryLine(
                juce::String("scan: EXCEPTION std::exception what=\"") + e.what() + "\"");
            err = "VST3 scan failed: " + juce::String(e.what());
            return {};
        }
        catch (...)
        {
            writeExperimentalInstrumentScanBoundaryLine("scan: EXCEPTION unknown (non-std)");
            err = "VST3 scan failed: unknown exception.";
            return {};
        }
    }

    [[nodiscard]] double effectiveSr(const double sr) noexcept
    {
        return sr > 0.0 ? sr : 48000.0;
    }

    [[nodiscard]] int effectiveBs(const int bs) noexcept
    {
        return bs > 0 ? bs : 512;
    }

    /// [Audio thread] Sum first stereo pair of scratch into device (same mono blend rule as PlaybackEngine).
    void addFirstStereoBusToDeviceOutputs(const float* L,
                                         const float* R,
                                         int run,
                                         int numOutputChannels,
                                         float* const* outputChannelData,
                                         float gainL,
                                         float gainR) noexcept
    {
        if (L == nullptr || R == nullptr || run <= 0 || numOutputChannels <= 0 || outputChannelData == nullptr)
        {
            return;
        }
        const float gL = juce::jmax(0.0f, gainL);
        const float gR = juce::jmax(0.0f, gainR);
        if (numOutputChannels == 1)
        {
            float* d = outputChannelData[0];
            if (d != nullptr)
            {
                const float halfGL = 0.5f * gL;
                const float halfGR = 0.5f * gR;
                juce::FloatVectorOperations::addWithMultiply(d, L, halfGL, run);
                juce::FloatVectorOperations::addWithMultiply(d, R, halfGR, run);
            }
        }
        else
        {
            if (float* d0 = outputChannelData[0])
            {
                juce::FloatVectorOperations::addWithMultiply(d0, L, gL, run);
            }
            if (numOutputChannels >= 2)
            {
                if (float* d1 = outputChannelData[1])
                {
                    juce::FloatVectorOperations::addWithMultiply(d1, R, gR, run);
                }
            }
        }
    }

    class ExperimentalPluginEditorWindow final : public juce::DocumentWindow
    {
    public:
        ExperimentalPluginEditorWindow(ExperimentalInstrumentHost& host,
                                         juce::AudioProcessor& proc,
                                         std::unique_ptr<juce::AudioProcessorEditor> editor)
            : DocumentWindow(proc.getName().isNotEmpty() ? proc.getName() : "Instrument",
                             juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                                 juce::ResizableWindow::backgroundColourId),
                             DocumentWindow::closeButton)
            , host_(host)
        {
            juce::ignoreUnused(proc);
            setUsingNativeTitleBar(true);
            setContentOwned(editor.release(), true);
            if (auto* c = getContentComponent())
            {
                c->addMouseListener(this, true);
                const int tw = juce::jmax(200, c->getWidth());
                const int th = juce::jmax(120, c->getHeight());
                centreWithSize(tw + 20, th + 20);
            }
            setVisible(true);
        }

        ~ExperimentalPluginEditorWindow() override
        {
            if (auto* c = getContentComponent())
            {
                c->removeMouseListener(this);
            }
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            juce::ignoreUnused(e);
            host_.messageThreadOnNativeEditorUserActivity();
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            juce::ignoreUnused(e);
            host_.messageThreadOnNativeEditorUserActivity();
        }

        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            juce::ignoreUnused(e, wheel);
            host_.messageThreadOnNativeEditorUserActivity();
        }

        void closeButtonPressed() override
        {
            setVisible(false);
            ExperimentalInstrumentHost* const h = &host_;
            juce::MessageManager::callAsync([h] {
                if (h != nullptr)
                {
                    h->editorWindowClosing();
                }
            });
        }

    private:
        ExperimentalInstrumentHost& host_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalPluginEditorWindow)
    };
} // namespace

struct ExperimentalInstrumentHost::MidiIoState
{
    juce::CriticalSection midiLock;
    /// Messages from UI with sample offset 0 (start of upcoming audio block).
    juce::MidiBuffer uiPendingMidi;
};

ExperimentalInstrumentHost::ExperimentalInstrumentHost()
    : midiIo_(std::make_unique<MidiIoState>())
{
    initExperimentalInstrumentSessionLog();
    formatManager_.addFormat(new juce::VST3PluginFormat());
    auto empty = std::shared_ptr<InstrumentOwner>{};
    std::atomic_store_explicit(&activeOwner_, empty, std::memory_order_release);
}

ExperimentalInstrumentHost::~ExperimentalInstrumentHost()
{
    onPluginPitchNamesCacheMayHaveChanged_ = {};
    drumNamePhaseCAudioProbeShouldSkip_ = {};
    unloadInstrument();
    writeExperimentalInstrumentLogLine("shutdown: ExperimentalInstrumentHost destroyed");
}

void ExperimentalInstrumentHost::closeNativeEditor()
{
    editorWindow_.reset();
}

void ExperimentalInstrumentHost::editorWindowClosing()
{
    writeExperimentalInstrumentLogLine("native editor: window closing (released)");
    editorWindow_.reset();
}

void ExperimentalInstrumentHost::queueMidiFromMessageThread(const ::juce::MidiMessage& message)
{
    if (midiIo_ == nullptr)
    {
        return;
    }
    const juce::ScopedLock sl(midiIo_->midiLock);
    midiIo_->uiPendingMidi.addEvent(message, 0);
}

void ExperimentalInstrumentHost::enqueueMidiMessageFromMessageThread(const juce::MidiMessage& message)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }
    if (!hasInstrument())
    {
        return;
    }
    queueMidiFromMessageThread(message);
}

void ExperimentalInstrumentHost::audioThread_beginAudioBlock(int numSamples) noexcept
{
    audioCallbackBlockSamples_ = juce::jmax(0, numSamples);
}

void ExperimentalInstrumentHost::audioThread_addMidiEventForCurrentBlock(int sampleOffsetInBlock,
                                                                      const juce::MidiMessage& message) noexcept
{
    const int cap = audioCallbackBlockSamples_;
    if (cap <= 0 || sampleOffsetInBlock < 0 || sampleOffsetInBlock >= cap)
    {
        rtTransportMidiAddEventDiscarded_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    rtBlockMidi_.addEvent(message, sampleOffsetInBlock);
}

std::uint64_t ExperimentalInstrumentHost::getTransportMidiAddEventDiscardedCountRelaxed() const noexcept
{
    return rtTransportMidiAddEventDiscarded_.load(std::memory_order_relaxed);
}

namespace
{
    [[nodiscard]] float decodeScratchPeakBits(std::uint32_t bits) noexcept
    {
        float v{};
        std::memcpy(&v, &bits, sizeof(float));
        return v;
    }

    [[nodiscard]] std::uint32_t encodeScratchPeak(float v) noexcept
    {
        std::uint32_t bits{};
        std::memcpy(&bits, &v, sizeof(float));
        return bits;
    }

    [[nodiscard]] juce::String joinSkipReasonTokens(const std::initializer_list<juce::String>& parts)
    {
        bool any = false;
        juce::String s("none");
        for (const juce::String& p : parts)
        {
            if (p.isEmpty())
                continue;
            if (!any)
            {
                s.clear();
                any = true;
                s << p;
            }
            else
            {
                s << "+" << p;
            }
        }
        return any ? s : juce::String("none");
    }
} // namespace

juce::String ExperimentalInstrumentHost::peekInstrumentAudioRoutingDiagLineForMessageThread() const noexcept
{
    try
    {
        auto ownerPeek = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
        const bool hi = ownerPeek != nullptr && ownerPeek->inst != nullptr && ownerPeek->layoutOk;

        const auto badIo = rtDiag_skipBadIoArgs_.load(std::memory_order_relaxed);
        const auto noOwn = rtDiag_skipNoOwnerBadLayout_.load(std::memory_order_relaxed);
        const auto lay = rtDiag_skipScratchLayout_.load(std::memory_order_relaxed);
        const auto smallCb = rtDiag_skipScratchTooSmallForCallback_.load(std::memory_order_relaxed);
        const auto noMidiIo = rtDiag_skipNoMidiIo_.load(std::memory_order_relaxed);
        const auto okBlk = rtDiag_processOkBlocks_.load(std::memory_order_relaxed);

        const int lastTot = rtDiag_lastTotalOut_.load(std::memory_order_relaxed);
        const int lastMain = rtDiag_lastMainOut_.load(std::memory_order_relaxed);
        const int lastScratchAllocCh = rtDiag_lastScratchChAllocated_.load(std::memory_order_relaxed);
        const int lastProcRun = rtDiag_lastProcessRun_.load(std::memory_order_relaxed);

        const float pk = decodeScratchPeakBits(rtDiag_lastScratchPeakBits_.load(std::memory_order_relaxed));
        const bool allZeroLast = rtDiag_lastScratchAllZero_.load(std::memory_order_relaxed) != 0;

        juce::String line = "instrument-audio: host=0x";
        line << juce::String::toHexString(
                    static_cast<juce::int64>(static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(
                        static_cast<const void*>(this)))))
             << " processCalled=" << juce::String(okBlk > 0 ? "yes" : "no") << " blocks=" << juce::String(okBlk)
             << " hasInstrument=" << juce::String(hi ? "yes" : "no") << " totalOut=" << juce::String(lastTot)
             << " mainOut=" << juce::String(lastMain) << " scratchChAlloc=" << juce::String(lastScratchAllocCh)
             << " lastN=" << juce::String(lastProcRun)
             << " pluginPeakLast=" << juce::String(pk, 9) << " allZeroLast=" << juce::String(allZeroLast ? "yes" : "no");

        line << " skips(badIo/noOwnerLayout/scratchLay/scratchSmall/noMidiIo)=" << juce::String(badIo) << '/'
             << juce::String(noOwn) << '/' << juce::String(lay) << '/' << juce::String(smallCb) << '/'
             << juce::String(noMidiIo);

        line << " skippedReason="
             << joinSkipReasonTokens({
                    badIo ? juce::String("badIoArgs") : juce::String(),
                    noOwn ? juce::String("noOwnerOrBadLayout") : juce::String(),
                    lay ? juce::String("scratchBusLayout") : juce::String(),
                    smallCb ? juce::String("scratchTooSmallForCallback") : juce::String(),
                    noMidiIo ? juce::String("noMidiIo") : juce::String(),
                });

        line << " sr=" << juce::String(sampleRate_, 3) << " blockSizeCfg=" << juce::String(blockSize_);

        return line;
    }
    catch (...)
    {
        return "instrument-audio: peek failed (exception)";
    }
}

void ExperimentalInstrumentHost::messageThreadOnNativeEditorUserActivity()
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

#if !MINIDAW_DIAG_PLAYBACK_ROUTING
    return;
#else
    const juce::int64 nowMs = juce::Time::currentTimeMillis();
    if (diagLastRoutingLogBumpMs_ != 0 && (nowMs - diagLastRoutingLogBumpMs_) < 750)
        return;
    diagLastRoutingLogBumpMs_ = nowMs;

    appendExperimentalPlaybackRoutingLogLine(juce::String("instrument-editor-activity: event=uiMouseOrWheel ")
                                               + peekInstrumentAudioRoutingDiagLineForMessageThread());
#endif
}

bool ExperimentalInstrumentHost::tryPrepareInstrumentLayout(juce::AudioPluginInstance& inst,
                                                            const double sampleRate,
                                                            const int blockSize)
{
    writeExperimentalInstrumentScanBoundaryLine(
        formatInstrumentHostThreadLine("bus negotiation: BEFORE " + summarizeProcessorBuses(inst)));

    const int beforeMainIn = inst.getMainBusNumInputChannels();
    const int beforeMainOut = inst.getMainBusNumOutputChannels();
    const bool acceptExisting = (beforeMainIn == 0 && beforeMainOut >= kStereoChannels);

    writeExperimentalInstrumentScanBoundaryLine(
        "bus negotiation: decision acceptExisting=" + juce::String(acceptExisting ? "true" : "false")
        + " beforeMainIn=" + juce::String(beforeMainIn) + " beforeMainOut=" + juce::String(beforeMainOut));

    const double srU = effectiveSr(sampleRate);
    const int bsU = effectiveBs(blockSize);

    if (!acceptExisting)
    {
        writeExperimentalInstrumentScanBoundaryLine("bus negotiation: BEFORE releaseResources (coerce path)");
        inst.releaseResources();
        writeExperimentalInstrumentScanBoundaryLine("bus negotiation: AFTER  releaseResources (coerce path)");

        writeExperimentalInstrumentScanBoundaryLine(
            "bus negotiation: BEFORE setPlayConfigDetails(0, stereo, sr, bs) (coerce path)");
        inst.setPlayConfigDetails(0, kStereoChannels, srU, bsU);
        writeExperimentalInstrumentScanBoundaryLine("bus negotiation: AFTER  setPlayConfigDetails (coerce path)");

        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add(juce::AudioChannelSet::disabled());
        layout.outputBuses.add(juce::AudioChannelSet::stereo());

        writeExperimentalInstrumentScanBoundaryLine(
            "bus negotiation: BEFORE setBusesLayout(0-in disabled, stereo main out)");
        const bool layoutSetOk = inst.setBusesLayout(layout);
        writeExperimentalInstrumentScanBoundaryLine(
            "bus negotiation: AFTER  setBusesLayout ok=" + juce::String(layoutSetOk ? "true" : "false"));

        if (!layoutSetOk)
        {
            writeExperimentalInstrumentScanBoundaryLine(
                "bus negotiation: BEFORE setPlayConfigDetails(stereo) recovery (coerce path)");
            inst.setPlayConfigDetails(0, kStereoChannels, srU, bsU);
            writeExperimentalInstrumentScanBoundaryLine(
                "bus negotiation: AFTER  setPlayConfigDetails recovery (coerce path)");
        }
    }
    else
    {
        writeExperimentalInstrumentScanBoundaryLine(
            "bus negotiation: SKIP setBusesLayout (accept existing instrument layout: 0-in, mainOut>=2)");
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "bus negotiation: BEFORE prepareToPlay sampleRate=" + juce::String(srU, 6)
        + " blockSize=" + juce::String(bsU));
    inst.prepareToPlay(srU, bsU);
    writeExperimentalInstrumentScanBoundaryLine("bus negotiation: AFTER  prepareToPlay");

    const int mainIn = inst.getMainBusNumInputChannels();
    const int mainOut = inst.getMainBusNumOutputChannels();
    const bool layoutOk = mainOut >= kStereoChannels;
    writeExperimentalInstrumentScanBoundaryLine(
        "bus negotiation: AFTER  mainIn=" + juce::String(mainIn) + " mainOut=" + juce::String(mainOut)
        + " " + summarizeProcessorBuses(inst) + " layoutOk=" + juce::String(layoutOk ? "true" : "false"));
    return layoutOk;
}

juce::Result ExperimentalInstrumentHost::loadInstrumentFromVst3File(const juce::File& vst3File)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return juce::Result::fail("Internal error: load must run on the message thread.");
    }
    if (!vst3File.exists())
    {
        return juce::Result::fail("VST3 file or bundle does not exist.");
    }

    unloadInstrument();

    writeExperimentalInstrumentScanBoundaryLine(
        "load: selected VST3 path=\"" + vst3File.getFullPathName() + "\"");

    juce::String err;
    const juce::PluginDescription desc = scanVst3FileAndLogDescriptions(vst3File, formatManager_, err);
    if (err.isNotEmpty() || desc.name.isEmpty())
    {
        writeExperimentalInstrumentLogLine(
            "load: scan failed err=\"" + (err.isNotEmpty() ? err : juce::String{ "empty description" }) + "\"");
        return juce::Result::fail(err.isNotEmpty() ? err : "Could not read VST3 description.");
    }

    juce::String chosenLine = "load: chosen PluginDescription name=\"" + desc.name + "\"";
    if (desc.descriptiveName.isNotEmpty())
    {
        chosenLine << " descriptiveName=\"" << desc.descriptiveName << "\"";
    }
    if (desc.pluginFormatName.isNotEmpty())
    {
        chosenLine << " format=\"" << desc.pluginFormatName << "\"";
    }
    if (desc.fileOrIdentifier.isNotEmpty())
    {
        chosenLine << " fileOrIdentifier=\"" << desc.fileOrIdentifier << "\"";
    }
    if (desc.category.isNotEmpty())
    {
        chosenLine << " category=\"" << desc.category << "\"";
    }
    if (desc.manufacturerName.isNotEmpty())
    {
        chosenLine << " manufacturer=\"" << desc.manufacturerName << "\"";
    }
    writeExperimentalInstrumentLogLine(chosenLine);

    const double srU = effectiveSr(sampleRate_);
    const int bsU = effectiveBs(blockSize_);

    writeExperimentalInstrumentScanBoundaryLine(
        formatInstrumentHostThreadLine("createPluginInstance: START sampleRate=" + juce::String(srU, 6)
                                       + " blockSize=" + juce::String(bsU)));

    std::unique_ptr<juce::AudioPluginInstance> inst;
    try
    {
        inst = formatManager_.createPluginInstance(desc, srU, bsU, err);
    }
    catch (const std::exception& e)
    {
        const juce::String msg = juce::String{ "Exception during createPluginInstance: " } + e.what();
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED exception=\"" + juce::String(e.what()) + "\"");
        return juce::Result::fail(msg);
    }
    catch (...)
    {
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED exception=(unknown)");
        return juce::Result::fail("Unknown exception during createPluginInstance.");
    }

    if (inst == nullptr)
    {
        const juce::String juceErr = err.isNotEmpty() ? err : juce::String{ "createPluginInstance failed (no message)." };
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED juceError=\"" + juceErr + "\"");
        return juce::Result::fail(err.isNotEmpty() ? err : "createPluginInstance failed.");
    }

    writeExperimentalInstrumentLogLine(
        "createPluginInstance: OK instanceName=\"" + inst->getName() + "\"");

    const bool layoutOk = tryPrepareInstrumentLayout(*inst, sampleRate_, blockSize_);
    if (!layoutOk)
    {
        writeExperimentalInstrumentLogLine("load: FAILED bus layout after prepare (see bus negotiation lines above).");
        inst->releaseResources();
        return juce::Result::fail(
            "Could not use instrument bus layout (need at least stereo main output). "
            "Plugin reports main bus output channels: "
            + juce::String(inst->getMainBusNumOutputChannels()));
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "load: BEFORE active slot publish + scratch alloc totalOut=" + juce::String(inst->getTotalNumOutputChannels()));

    auto owner = std::make_shared<InstrumentOwner>();
    owner->inst = std::move(inst);
    owner->layoutOk = true;

    std::atomic_store_explicit(&activeOwner_, owner, std::memory_order_release);

    const int totalCh = owner->inst->getTotalNumOutputChannels();
    const int scratchCh = juce::jmax(kStereoChannels, totalCh);
    const int bsRowsForScratch = effectiveBs(blockSize_);
    scratch_.setSize(scratchCh, bsRowsForScratch, false, true, true);
    scratchPtrs_.clear();
    scratchPtrs_.reserve((size_t)scratchCh);
    for (int c = 0; c < scratchCh; ++c)
    {
        scratchPtrs_.push_back(scratch_.getWritePointer(c));
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "load: AFTER  active slot publish + scratch alloc scratchCh=" + juce::String(scratchCh)
        + " scratchRows=" + juce::String(bsRowsForScratch) + " blockSizeCfg=" + juce::String(blockSize_));

    juce::Logger::writeToLog(
        juce::String{ "[experimental-instrument] loaded " } + owner->inst->getName()
        + " totalOutCh=" + juce::String(totalCh));

    writeExperimentalInstrumentLogLine(
        "load: COMPLETED OK plugin=\"" + owner->inst->getName() + "\" totalOutCh=" + juce::String(totalCh));

    lastLoadedVst3OriginalPath_ = vst3File.getFullPathName();
    lastLoadedPluginDescription_ = desc;
    lastLoadedPluginDescriptionValid_ = true;
    return juce::Result::ok();
}

juce::String ExperimentalInstrumentHost::getLastLoadedVst3OriginalPath() const noexcept
{
    return lastLoadedVst3OriginalPath_;
}

bool ExperimentalInstrumentHost::getLastLoadedPluginDescription(juce::PluginDescription& out) const noexcept
{
    if (!lastLoadedPluginDescriptionValid_)
    {
        return false;
    }
    out = lastLoadedPluginDescription_;
    return true;
}

void ExperimentalInstrumentHost::appendInstrumentHostLogLine(const juce::String& message)
{
    writeExperimentalInstrumentLogLine(message);
}

juce::String ExperimentalInstrumentHost::getCurrentInstrumentStateBase64() const
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return {};
    }

    writeExperimentalInstrumentLogLine("plugin-state: save begin");

    auto o = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (o == nullptr || o->inst == nullptr || !o->layoutOk)
    {
        writeExperimentalInstrumentLogLine("plugin-state: save skipped reason=no-instrument");
        return {};
    }

    juce::MemoryBlock mb;
    o->inst->getStateInformation(mb);
    const juce::int64 nBytes = (juce::int64)mb.getSize();
    if (nBytes <= 0)
    {
        writeExperimentalInstrumentLogLine("plugin-state: save ok bytes=0 base64Chars=0");
        return {};
    }

    const juce::String b64 = juce::Base64::toBase64(mb.getData(), (int)mb.getSize());
    writeExperimentalInstrumentLogLine("plugin-state: save ok bytes=" + juce::String(nBytes)
                                       + " base64Chars=" + juce::String(b64.length()));
    return b64;
}

juce::Result ExperimentalInstrumentHost::loadInstrumentFromDescription(const juce::PluginDescription& descIn,
                                                                         const juce::File& originalPath,
                                                                         const char* sourceTag,
                                                                         const juce::MemoryBlock* pluginStateToRestore,
                                                                         juce::String* outPluginStateRestoreWarning)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return juce::Result::fail("Internal error: load must run on the message thread.");
    }
    if (!originalPath.exists())
    {
        return juce::Result::fail("VST3 file or bundle does not exist.");
    }

    unloadInstrument();

    juce::PluginDescription desc = descIn;
    if (desc.fileOrIdentifier.isEmpty())
    {
        desc.fileOrIdentifier = originalPath.getFullPathName();
    }

    const juce::String sourceStr
        = (sourceTag != nullptr && sourceTag[0] != '\0') ? juce::String(sourceTag) : juce::String("oop-description");
    writeExperimentalInstrumentLogLine(
        "load: source=" + sourceStr + " name=\"" + desc.name + "\" manufacturer=\"" + desc.manufacturerName
        + "\" format=\"" + desc.pluginFormatName + "\" fileOrIdentifier=\"" + desc.fileOrIdentifier
        + "\" originalPath=\"" + originalPath.getFullPathName() + "\"");

    juce::String chosenLine = "load: chosen PluginDescription name=\"" + desc.name + "\"";
    if (desc.descriptiveName.isNotEmpty())
    {
        chosenLine << " descriptiveName=\"" << desc.descriptiveName << "\"";
    }
    if (desc.pluginFormatName.isNotEmpty())
    {
        chosenLine << " format=\"" << desc.pluginFormatName << "\"";
    }
    if (desc.fileOrIdentifier.isNotEmpty())
    {
        chosenLine << " fileOrIdentifier=\"" << desc.fileOrIdentifier << "\"";
    }
    if (desc.category.isNotEmpty())
    {
        chosenLine << " category=\"" << desc.category << "\"";
    }
    if (desc.manufacturerName.isNotEmpty())
    {
        chosenLine << " manufacturer=\"" << desc.manufacturerName << "\"";
    }
    writeExperimentalInstrumentLogLine(chosenLine);

    const double srU = effectiveSr(sampleRate_);
    const int bsU = effectiveBs(blockSize_);

    writeExperimentalInstrumentScanBoundaryLine(
        formatInstrumentHostThreadLine("createPluginInstance: START sampleRate=" + juce::String(srU, 6)
                                       + " blockSize=" + juce::String(bsU)));

    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> inst;
    try
    {
        inst = formatManager_.createPluginInstance(desc, srU, bsU, err);
    }
    catch (const std::exception& e)
    {
        const juce::String msg = juce::String{ "Exception during createPluginInstance: " } + e.what();
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED exception=\"" + juce::String(e.what()) + "\"");
        return juce::Result::fail(msg);
    }
    catch (...)
    {
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED exception=(unknown)");
        return juce::Result::fail("Unknown exception during createPluginInstance.");
    }

    if (inst == nullptr)
    {
        const juce::String juceErr = err.isNotEmpty() ? err : juce::String{ "createPluginInstance failed (no message)." };
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED juceError=\"" + juceErr + "\"");
        return juce::Result::fail(err.isNotEmpty() ? err : "createPluginInstance failed.");
    }

    writeExperimentalInstrumentLogLine(
        "createPluginInstance: OK instanceName=\"" + inst->getName() + "\"");

    bool finalLayoutOk = tryPrepareInstrumentLayout(*inst, sampleRate_, blockSize_);
    if (!finalLayoutOk)
    {
        writeExperimentalInstrumentLogLine("load: FAILED bus layout after prepare (see bus negotiation lines above).");
        inst->releaseResources();
        return juce::Result::fail(
            "Could not use instrument bus layout (need at least stereo main output). "
            "Plugin reports main bus output channels: "
            + juce::String(inst->getMainBusNumOutputChannels()));
    }

    if (pluginStateToRestore != nullptr && pluginStateToRestore->getSize() > 0)
    {
        writeExperimentalInstrumentLogLine(
            "plugin-state: restore begin bytes=" + juce::String((juce::int64)pluginStateToRestore->getSize()));
        bool restoreOk = false;
        try
        {
            inst->setStateInformation(pluginStateToRestore->getData(), (int)pluginStateToRestore->getSize());
            if (inst->getMainBusNumOutputChannels() < kStereoChannels)
            {
                inst->prepareToPlay(srU, bsU);
            }
            restoreOk = inst->getMainBusNumOutputChannels() >= kStereoChannels;
            if (restoreOk)
            {
                writeExperimentalInstrumentLogLine("plugin-state: restore ok");
            }
        }
        catch (const std::exception& e)
        {
            writeExperimentalInstrumentLogLine(
                "plugin-state: restore failed message=\"" + juce::String(e.what()) + "\"");
            if (outPluginStateRestoreWarning != nullptr)
            {
                *outPluginStateRestoreWarning
                    = "Groove Agent could not apply saved plug-in state (" + juce::String(e.what())
                      + "). You may need to load the kit manually if audio is silent.";
            }
        }
        catch (...)
        {
            writeExperimentalInstrumentLogLine("plugin-state: restore failed message=\"unknown exception\"");
            if (outPluginStateRestoreWarning != nullptr)
            {
                *outPluginStateRestoreWarning = "Groove Agent could not apply saved plug-in state (unknown error). "
                                                "You may need to load the kit manually if audio is silent.";
            }
        }

        if (!restoreOk && outPluginStateRestoreWarning != nullptr && outPluginStateRestoreWarning->isEmpty())
        {
            writeExperimentalInstrumentLogLine(
                "plugin-state: restore failed message=\"invalid output bus layout after setState\"");
            *outPluginStateRestoreWarning
                = "Groove Agent saved state could not be applied (invalid bus layout after restore). "
                  "You may need to load the kit manually if audio is silent.";
        }

        if (!restoreOk)
        {
            writeExperimentalInstrumentLogLine("plugin-state: reset to default patch after failed restore");
            inst->releaseResources();
            finalLayoutOk = tryPrepareInstrumentLayout(*inst, sampleRate_, blockSize_);
            if (!finalLayoutOk)
            {
                writeExperimentalInstrumentLogLine(
                    "load: FAILED bus layout after failed state restore + reset (see bus negotiation lines above).");
                inst->releaseResources();
                return juce::Result::fail(
                    "Could not use instrument bus layout after attempted state restore reset. "
                    "Plugin reports main bus output channels: "
                    + juce::String(inst->getMainBusNumOutputChannels()));
            }
        }
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "load: BEFORE active slot publish + scratch alloc totalOut=" + juce::String(inst->getTotalNumOutputChannels()));

    auto owner = std::make_shared<InstrumentOwner>();
    owner->inst = std::move(inst);
    owner->layoutOk = finalLayoutOk;

    std::atomic_store_explicit(&activeOwner_, owner, std::memory_order_release);

    const int totalCh = owner->inst->getTotalNumOutputChannels();
    const int scratchCh = juce::jmax(kStereoChannels, totalCh);
    const int bsRowsForScratch = effectiveBs(blockSize_);
    scratch_.setSize(scratchCh, bsRowsForScratch, false, true, true);
    scratchPtrs_.clear();
    scratchPtrs_.reserve((size_t)scratchCh);
    for (int c = 0; c < scratchCh; ++c)
    {
        scratchPtrs_.push_back(scratch_.getWritePointer(c));
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "load: AFTER  active slot publish + scratch alloc scratchCh=" + juce::String(scratchCh)
        + " scratchRows=" + juce::String(bsRowsForScratch) + " blockSizeCfg=" + juce::String(blockSize_));

    juce::Logger::writeToLog(
        juce::String{ "[experimental-instrument] loaded " } + owner->inst->getName()
        + " totalOutCh=" + juce::String(totalCh));

    writeExperimentalInstrumentLogLine(
        "load: COMPLETED OK plugin=\"" + owner->inst->getName() + "\" totalOutCh=" + juce::String(totalCh));

    lastLoadedVst3OriginalPath_ = originalPath.getFullPathName();
    lastLoadedPluginDescription_ = desc;
    lastLoadedPluginDescriptionValid_ = true;
    return juce::Result::ok();
}

juce::Result ExperimentalInstrumentHost::diagnosticScanVst3FileOnly(const juce::File& vst3File)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return juce::Result::fail("Internal error: scan diagnostic must run on the message thread.");
    }
    if (!vst3File.exists())
    {
        return juce::Result::fail("VST3 file or bundle does not exist.");
    }

    const juce::String pathOrId = vst3File.getFullPathName();
    writeExperimentalInstrumentLogLine(
        "SCAN_DIAGNOSTIC: scan-only (no instance); details in experimental-vst3-scan-diagnostic.log path=\""
        + pathOrId + "\"");

    writeScanDiagnosticScanBoundaryLine("=== SCAN_DIAGNOSTIC run begin path=\"" + pathOrId + "\" ===");
    writeScanDiagnosticScanBoundaryLine(formatInstrumentHostThreadLine("SCAN_DIAGNOSTIC: context"));

    juce::OwnedArray<juce::PluginDescription> list;
    try
    {
        runVst3FindAllTypesForFileLoop(
            pathOrId,
            formatManager_,
            list,
            [](const juce::String& msg) { writeScanDiagnosticScanBoundaryLine(msg); });

        juce::String summary = "SCAN_DIAGNOSTIC run end outcome=completed reachedAfterAllFormats=true descriptionCount="
                               + juce::String(list.size());
        for (int i = 0; i < list.size(); ++i)
        {
            const juce::PluginDescription* d = list[i];
            if (d == nullptr)
            {
                summary << " [" << i << "]=null";
            }
            else
            {
                summary << " [" << i << "]=\"" << d->name << "\"";
            }
        }
        writeScanDiagnosticScanBoundaryLine(summary + " path=\"" + pathOrId + "\"");
        return juce::Result::ok();
    }
    catch (const std::exception& e)
    {
        writeScanDiagnosticScanBoundaryLine(
            juce::String("SCAN_DIAGNOSTIC run end outcome=exception std::exception=\"") + e.what() + "\" path=\""
            + pathOrId + "\"");
        return juce::Result::fail(juce::String("Scan threw: ") + e.what());
    }
    catch (...)
    {
        writeScanDiagnosticScanBoundaryLine(
            "SCAN_DIAGNOSTIC run end outcome=exception unknown path=\"" + pathOrId + "\"");
        return juce::Result::fail("Scan threw a non-std exception.");
    }
}

void ExperimentalInstrumentHost::unloadInstrument()
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    writeExperimentalInstrumentLogLine("unload: requested");

    rawPluginPitchNamesByNote_.clear();
    pluginPitchNamesByNote_.clear();
    pluginDrumNameMapAuthoritative_ = false;
    lastGrooveDrumCapabilityPersistKey_.clear();
    writeDrumNameDiagnosticLogLine("drum-names: cleared (instrument unload)");

    lastLoadedVst3OriginalPath_.clear();
    lastLoadedPluginDescriptionValid_ = false;
    lastLoadedPluginDescription_ = {};
    drumNamePhaseCPendingAfterEditorOpen_ = false;
    primaryPadDisplayActiveNotes_.clear();

    diagLastRoutingLogBumpMs_ = 0;

    rtDiag_skipBadIoArgs_.store(0, std::memory_order_relaxed);
    rtDiag_skipNoOwnerBadLayout_.store(0, std::memory_order_relaxed);
    rtDiag_skipScratchLayout_.store(0, std::memory_order_relaxed);
    rtDiag_skipScratchTooSmallForCallback_.store(0, std::memory_order_relaxed);
    rtDiag_skipNoMidiIo_.store(0, std::memory_order_relaxed);
    rtDiag_processOkBlocks_.store(0, std::memory_order_relaxed);
    rtDiag_lastTotalOut_.store(0, std::memory_order_relaxed);
    rtDiag_lastMainOut_.store(0, std::memory_order_relaxed);
    rtDiag_lastScratchChAllocated_.store(0, std::memory_order_relaxed);
    rtDiag_lastProcessRun_.store(0, std::memory_order_relaxed);
    rtDiag_lastScratchPeakBits_.store(0, std::memory_order_relaxed);
    rtDiag_lastScratchAllZero_.store(1, std::memory_order_relaxed);

    closeNativeEditor();

    std::shared_ptr<InstrumentOwner> prev = std::atomic_exchange_explicit(
        &activeOwner_, std::shared_ptr<InstrumentOwner>{}, std::memory_order_acq_rel);

    juce::Thread::sleep(100);

    if (prev != nullptr && prev->inst != nullptr)
    {
        prev->inst->releaseResources();
    }

    prev.reset();
    scratch_.setSize(0, 0, false, true, false);
    scratchPtrs_.clear();

    juce::Logger::writeToLog("[experimental-instrument] unloaded (global slot)");

    writeExperimentalInstrumentLogLine("unload: completed");
}

bool ExperimentalInstrumentHost::hasInstrument() const noexcept
{
    auto o = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    return o != nullptr && o->inst != nullptr && o->layoutOk;
}

juce::String ExperimentalInstrumentHost::getInstrumentNameForUi() const
{
    auto o = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (o == nullptr || o->inst == nullptr)
    {
        return {};
    }
    return o->inst->getName();
}

void ExperimentalInstrumentHost::setOnPluginPitchNamesCacheMayHaveChanged(std::function<void()> callback)
{
    onPluginPitchNamesCacheMayHaveChanged_ = std::move(callback);
}

void ExperimentalInstrumentHost::setOnPluginDrumNamesDiscovered(
    std::function<void(const std::map<int, juce::String>&)> callback)
{
    onPluginDrumNamesDiscovered_ = std::move(callback);
}

void ExperimentalInstrumentHost::setDrumNamePhaseCAudioProbeShouldSkip(std::function<bool()> shouldSkip)
{
    drumNamePhaseCAudioProbeShouldSkip_ = std::move(shouldSkip);
}

void ExperimentalInstrumentHost::refreshPluginNoteNamesFromActiveInstrument()
{
    // Not invoked automatically in production (load / rescan / editor). Reserved for future explicit
    // "Import names from plugin" and for `kDrumNamesDiag` scheduling paths.
    refreshPluginNoteNamesFromActiveInstrumentImpl(drum_name_diag::DrumNameRefreshPhase::immediate, true);
}

// Production code must not call this for autoload/rescan; global cached capabilities are not authoritative.
void ExperimentalInstrumentHost::seedDrumDisplayFromCachedCapability(const mini_daw::PluginCapabilities& caps)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    auto o = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (o == nullptr || o->inst == nullptr || !o->layoutOk)
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine(
            "cached drum capability seed: skipped reason=no_active_instrument");
        return;
    }

    if (!o->inst->getName().containsIgnoreCase("Groove Agent"))
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine("cached drum capability seed: skipped reason=not_groove_agent");
        return;
    }

    if (!caps.drumNoteDisplay.has_value())
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine(
            "cached drum capability seed: skipped reason=missing_drumNoteDisplay");
        return;
    }

    const auto& dnd = *caps.drumNoteDisplay;
    if (!dnd.confidence.equalsIgnoreCase("high"))
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine("cached drum capability seed: skipped reason=low_confidence confidence=\""
                                                    + dnd.confidence + "\"");
        return;
    }

    int validActive = 0;
    for (const auto& an : dnd.activeNotes)
    {
        if (an.midi >= 0 && an.midi <= 127)
        {
            ++validActive;
        }
    }
    if (validActive == 0)
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine("cached drum capability seed: skipped reason=empty_active_notes");
        return;
    }

    try
    {
        rawPluginPitchNamesByNote_.clear();
        if (caps.rawPitchMap.has_value())
        {
            for (const auto& e : caps.rawPitchMap->notes)
            {
                if (e.midi >= 0 && e.midi <= 127 && e.name.isNotEmpty())
                {
                    rawPluginPitchNamesByNote_[e.midi] = e.name;
                }
            }
        }

        pluginPitchNamesByNote_.clear();
        primaryPadDisplayActiveNotes_.clear();
        for (const auto& an : dnd.activeNotes)
        {
            if (an.midi < 0 || an.midi > 127)
            {
                continue;
            }
            primaryPadDisplayActiveNotes_.insert(an.midi);
            juce::String label = an.rawName;
            if (label.isEmpty())
            {
                const auto it = rawPluginPitchNamesByNote_.find(an.midi);
                if (it != rawPluginPitchNamesByNote_.end())
                {
                    label = it->second;
                }
            }
            if (label.isNotEmpty())
            {
                pluginPitchNamesByNote_[an.midi] = label;
            }
        }

        pluginDrumNameMapAuthoritative_ = true;

        const int rawCount = (int)rawPluginPitchNamesByNote_.size();
        const int displayCount = (int)pluginPitchNamesByNote_.size();

        mini_daw::writeVst3OopScanDiagnosticLogLine("cached drum capability seeded rawCount=" + juce::String(rawCount)
                                                    + " displayCount=" + juce::String(displayCount) + " activePadCount="
                                                    + juce::String((int)primaryPadDisplayActiveNotes_.size()));

        if (onPluginPitchNamesCacheMayHaveChanged_ != nullptr)
        {
            onPluginPitchNamesCacheMayHaveChanged_();
        }
    }
    catch (...)
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine("cached drum capability seed: skipped reason=exception");
    }
}

void ExperimentalInstrumentHost::scheduleDrumNameDiagLifecyclePhasesAfterRefreshIfEnabled()
{
    if (!drum_name_diag::kDrumNamesDiag)
    {
        return;
    }

    auto* const self = this;
    juce::Timer::callAfterDelay(250, [self] {
        if (self == nullptr || !drum_name_diag::kDrumNamesDiag)
        {
            return;
        }
        self->refreshPluginNoteNamesFromActiveInstrumentImpl(drum_name_diag::DrumNameRefreshPhase::delayed250, false);
    });
    juce::Timer::callAfterDelay(1000, [self] {
        if (self == nullptr || !drum_name_diag::kDrumNamesDiag)
        {
            return;
        }
        self->refreshPluginNoteNamesFromActiveInstrumentImpl(drum_name_diag::DrumNameRefreshPhase::delayed1000, false);
    });
}

void ExperimentalInstrumentHost::schedulePluginPitchNamesRefreshAfterNativeEditorOpened()
{
    drumNamePhaseCPendingAfterEditorOpen_ = true;
    auto* const self = this;
    juce::Timer::callAfterDelay(150, [self] {
        if (self == nullptr)
        {
            return;
        }
        // Production: transient host pitch-name caches are diagnostic-only (`kDrumNamesDiag`).
        // Track-local `autoPlugin` labels always use the deferred `afterEditorOpen` probe (see discovery block in impl).
        self->refreshPluginNoteNamesFromActiveInstrumentImpl(drum_name_diag::DrumNameRefreshPhase::afterEditorOpen,
                                                            drum_name_diag::kDrumNamesDiag);
    });
}

void ExperimentalInstrumentHost::refreshPluginNoteNamesFromActiveInstrumentImpl(
    const drum_name_diag::DrumNameRefreshPhase phase,
    const bool updateTransientNameCache)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    auto o = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (o == nullptr || o->inst == nullptr || !o->layoutOk)
    {
        writeDrumNameDiagnosticLogLine(
            juce::String("drum-names: refresh skipped reason=active instrument missing or layout not ok phase=")
            + drum_name_diag::drumNameRefreshPhaseTag(phase));
        return;
    }

    if (updateTransientNameCache)
    {
        rawPluginPitchNamesByNote_.clear();
        pluginPitchNamesByNote_.clear();
        primaryPadDisplayActiveNotes_.clear();
        pluginDrumNameMapAuthoritative_ = false;
    }

    std::map<int, juce::String> rawMap;
    experimental_instrument_host_detail::DrumNameVst3ProbeDetails probe{};
    const std::function<void(const juce::String&)> drumNameVerboseSink = [](const juce::String& line) {
        writeDrumNameDiagnosticLogLine(line);
    };
    const std::function<void(const juce::String&)>* verbosePtr
        = drum_name_diag::kDrumNamesDiag ? &drumNameVerboseSink : nullptr;
    const int nRaw = experimental_instrument_host_detail::tryFillPitchNamesFromVst3UnitInfo(
        *o->inst,
        rawMap,
        probe,
        verbosePtr);
    const juce::String instName = o->inst->getName();

    const PluginDrumMapAuthorityEval authorityEval = evaluatePluginDrumMapAuthority(phase, nRaw, rawMap);

    const DerivePrimaryPadDisplayResult derivedPads = derivePrimaryDrumPadDisplayNotesFromRawMap(rawMap);
    std::map<int, juce::String> displayScratch;
    juce::String displaySourceStr;
    juce::String derivationLogReason = derivedPads.reason;
    if (derivedPads.ok)
    {
        buildPrimaryPadDisplayMapFromNoteSet(rawMap, displayScratch, derivedPads.notes);
        displaySourceStr = "metadataDerived";
    }
    else
    {
        buildPrimaryPadDisplayMap(
            rawMap,
            displayScratch,
            ExperimentalInstrumentHost::kFallbackPrimaryDrumPadDisplayMin,
            ExperimentalInstrumentHost::kFallbackPrimaryDrumPadDisplayMax);
        displaySourceStr = "fallbackRange";
        if (derivationLogReason.isEmpty())
        {
            derivationLogReason = "fallback";
        }
    }
    const int nDisplayFromProbe = (int)displayScratch.size();

    if (phase == drum_name_diag::DrumNameRefreshPhase::afterEditorOpen)
    {
        std::map<int, juce::String> forTrack;
        for (const auto& kv : displayScratch)
        {
            juce::String t = kv.second.trim();
            if (t.isNotEmpty())
                forTrack.emplace(kv.first, std::move(t));
        }
        if (forTrack.empty())
        {
            juce::String reason;
            if (nRaw <= 0)
                reason = probe.zeroReason.isNotEmpty() ? ("reason=probe_empty detail=\"" + probe.zeroReason + "\"")
                                                       : juce::String{"reason=probe_empty detail=none"};
            else
                reason = juce::String{"reason=no_non_empty_display_names rawCount="} + juce::String{nRaw}
                         + juce::String{" derivedOk="}
                         + juce::String{derivedPads.ok ? "true" : "false"}
                         + juce::String{" displaySource="} + displaySourceStr;

            appendInstrumentHostLogLine(
                juce::String{"drum-map: plugin names skipped "} + reason + juce::String{" trigger=afterEditorOpen"});
        }
        else
        {
            appendInstrumentHostLogLine(
                juce::String{"drum-map: plugin names discovered source=loaded_plugin count="}
                + juce::String(static_cast<int>(forTrack.size())) + juce::String{" trigger=afterEditorOpen"});
            if (onPluginDrumNamesDiscovered_)
            {
                onPluginDrumNamesDiscovered_(forTrack);
            }
        }
    }

    juce::String authorityReasonForLog;
    if (updateTransientNameCache)
    {
        rawPluginPitchNamesByNote_ = std::move(rawMap);
        pluginPitchNamesByNote_ = std::move(displayScratch);
        if (derivedPads.ok)
        {
            primaryPadDisplayActiveNotes_ = derivedPads.notes;
        }
        else
        {
            primaryPadDisplayActiveNotes_
                = fallbackPrimaryPadNoteSetFromRaw(rawPluginPitchNamesByNote_,
                                                   ExperimentalInstrumentHost::kFallbackPrimaryDrumPadDisplayMin,
                                                   ExperimentalInstrumentHost::kFallbackPrimaryDrumPadDisplayMax);
        }
        pluginDrumNameMapAuthoritative_ = authorityEval.authoritative;
        authorityReasonForLog = authorityEval.reason;

        if (pluginDrumNameMapAuthoritative_
            && phase != drum_name_diag::DrumNameRefreshPhase::afterEditorOpen)
        {
            maybePersistGrooveDrumCapabilitiesToV2Cache(probe, derivationLogReason);
        }
    }
    else
    {
        authorityReasonForLog = "transient_cache_not_updated eval_if_applied_would_be=\""
                                + authorityEval.reason + "\" currentAuthoritative="
                                + juce::String(pluginDrumNameMapAuthoritative_ ? "true" : "false");
    }

    if (drum_name_diag::kDrumNamesDiag)
    {
        const int nDisplayForSummary
            = updateTransientNameCache ? (int)pluginPitchNamesByNote_.size() : nDisplayFromProbe;

        juce::String displayRangeStr = "empty";
        if (!displayScratch.empty())
        {
            displayRangeStr = juce::String(displayScratch.begin()->first) + "-"
                              + juce::String(displayScratch.rbegin()->first);
        }

        const drum_name_diag::PluginDrumNameMapTrust mapTrust
            = trustLevelForMap(pluginDrumNameMapAuthoritative_, pluginPitchNamesByNote_);

        juce::String programSelection = "selectedListId=none programIndex=none programName=\"\"";
        if (probe.selectedListId >= 0 && probe.selectedProgramIndex >= 0)
        {
            programSelection = "selectedListId=" + juce::String(probe.selectedListId)
                               + " programIndex=" + juce::String(probe.selectedProgramIndex);
            if (probe.selectedProgramName.isNotEmpty())
            {
                programSelection << " programName=\"" << probe.selectedProgramName << "\"";
            }
            else
            {
                programSelection << " programName=\"\"";
            }
        }

        juce::String summary = "drum-names: summary phase=" + juce::String(drum_name_diag::drumNameRefreshPhaseTag(phase))
                               + " instance=\"" + instName + "\" rawNamesFound=" + juce::String(nRaw)
                               + " displayNamesFound=" + juce::String(nDisplayForSummary)
                               + " displayRange=" + displayRangeStr + " displaySource=" + displaySourceStr
                               + " derivationReason=\"" + derivationLogReason + "\" derivedDisplayNotes="
                               + formatIntSetForDiagLog(derivedPads.ok ? derivedPads.notes : std::set<int>{})
                               + " pluginMapAuthoritative=" + juce::String(pluginDrumNameMapAuthoritative_ ? "true" : "false")
                               + " mapTrust=" + juce::String(drum_name_diag::pluginDrumNameMapTrustTag(mapTrust))
                               + " authorityReason=\"" + authorityReasonForLog + "\" " + programSelection
                               + " vst3IComponent=" + juce::String(probe.vst3IComponentPresent ? "true" : "false")
                               + " editController=" + juce::String(probe.editControllerPresent ? "true" : "false")
                               + " unitInfoSource=" + probe.unitInfoSource
                               + " activeProgramIndex=" + juce::String(probe.activeProgramIndex)
                               + " programLists=" + juce::String(probe.programListCount);
        summary << " unitCount=" << juce::String(probe.unitCount) << " selectedUnit=" << probe.selectedUnitIdStr
                << " keyswitchController=" << juce::String(probe.keyswitchControllerPresent ? "true" : "false")
                << " keyswitchCountBus0Ch0=" << juce::String(probe.keyswitchCountBus0Ch0)
                << " keyswitchCountBus0Ch9=" << juce::String(probe.keyswitchCountBus0Ch9);
        if (nRaw == 0 && probe.zeroReason.isNotEmpty())
        {
            summary << " reason=\"" << probe.zeroReason << "\"";
        }
        writeDrumNameDiagnosticLogLine(summary);

        writeDrumNameDiagnosticLogLine("drum-names: enabled");
        writeDrumNameDiagnosticLogLine("drum-names: refreshing plugin names phase="
                                       + juce::String(drum_name_diag::drumNameRefreshPhaseTag(phase)) + " instance=\""
                                       + instName + "\" updateTransientNameCache="
                                       + juce::String(updateTransientNameCache ? "true" : "false"));
        writeDrumNameDiagnosticLogLine("drum-names: vst3IComponent="
                                       + juce::String(probe.vst3IComponentPresent ? "true" : "false"));
        writeDrumNameDiagnosticLogLine("drum-names: editController="
                                       + juce::String(probe.editControllerPresent ? "true" : "false"));
        writeDrumNameDiagnosticLogLine("drum-names: unitInfoSource=" + probe.unitInfoSource);
        writeDrumNameDiagnosticLogLine("drum-names: activeProgramIndex=" + juce::String(probe.activeProgramIndex));
        writeDrumNameDiagnosticLogLine("drum-names: programLists=" + juce::String(probe.programListCount));
        if (probe.selectedListId >= 0 && probe.selectedProgramIndex >= 0)
        {
            juce::String sel = "drum-names: selected listId=" + juce::String(probe.selectedListId)
                               + " programIndex=" + juce::String(probe.selectedProgramIndex);
            if (probe.selectedProgramName.isNotEmpty())
            {
                sel << " programName=\"" << probe.selectedProgramName << "\"";
            }
            writeDrumNameDiagnosticLogLine(sel);
        }
        else
        {
            writeDrumNameDiagnosticLogLine("drum-names: selected listId=none programIndex=none");
        }
        writeDrumNameDiagnosticLogLine("drum-names: rawNamesFound=" + juce::String(nRaw) + " displayNamesFound="
                                         + juce::String(nDisplayForSummary) + " displayRange=" + displayRangeStr
                                         + " displaySource=" + displaySourceStr + " derivationReason=\""
                                         + derivationLogReason + "\" derivedDisplayNotes="
                                         + formatIntSetForDiagLog(derivedPads.ok ? derivedPads.notes : std::set<int>{}));
        writeDrumNameDiagnosticLogLine("drum-names: getProgramPitchNameOkCalls="
                                       + juce::String(probe.getProgramPitchNameOkCount));
        if (probe.zeroReason.isNotEmpty())
        {
            writeDrumNameDiagnosticLogLine("drum-names: zeroReason=\"" + probe.zeroReason + "\"");
        }
        const std::map<int, juce::String>& rawForDiag
            = updateTransientNameCache ? rawPluginPitchNamesByNote_ : rawMap;
        const std::map<int, juce::String>& displayForDiag
            = updateTransientNameCache ? pluginPitchNamesByNote_ : displayScratch;
        constexpr int chLog = 10;
        for (int note = 24; note <= 84; ++note)
        {
            const auto it = rawForDiag.find(note);
            const juce::String rawN = (it != rawForDiag.end()) ? it->second : juce::String{};
            const juce::String gm = drum_note_names::gmPercussionName(note);
            if (note <= 55)
            {
                const auto pIt = displayForDiag.find(note);
                const juce::String displayN
                    = (pIt != displayForDiag.end() && pIt->second.isNotEmpty()) ? pIt->second : juce::String{};
                writeDrumNameDiagnosticLogLine("drum-names: note=" + juce::String(note) + " ch="
                                               + juce::String(chLog) + " raw=\"" + rawN + "\" display=\"" + displayN
                                               + "\" gm=\"" + gm + "\"");
            }
            else
            {
                writeDrumNameDiagnosticLogLine("drum-names: note=" + juce::String(note) + " ch="
                                               + juce::String(chLog) + " raw=\"" + rawN + "\" gm=\"" + gm + "\"");
            }
        }
    }

    if (updateTransientNameCache && drum_name_diag::kDrumNamesDiag)
    {
        scheduleDrumNameDiagLifecyclePhasesAfterRefreshIfEnabled();
    }

    if (phase == drum_name_diag::DrumNameRefreshPhase::afterEditorOpen && updateTransientNameCache
        && onPluginPitchNamesCacheMayHaveChanged_)
    {
        onPluginPitchNamesCacheMayHaveChanged_();
    }

    if (phase == drum_name_diag::DrumNameRefreshPhase::afterEditorOpen && updateTransientNameCache && o != nullptr
        && o->inst != nullptr && o->layoutOk)
    {
        if (drumNamePhaseCPendingAfterEditorOpen_)
        {
            drumNamePhaseCPendingAfterEditorOpen_ = false;
            if (drum_name_diag::kDrumNamesDiag)
            {
                runDrumNamePhaseCDiagnosticsIfEligible(phase, updateTransientNameCache, *o->inst);
            }
        }
    }
}

void ExperimentalInstrumentHost::maybePersistGrooveDrumCapabilitiesToV2Cache(
    const experimental_instrument_host_detail::DrumNameVst3ProbeDetails& probe,
    const juce::String& derivationLogReason)
{
    if (!kPersistGlobalDrumCapabilityHints)
    {
        return;
    }

    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    if (!pluginDrumNameMapAuthoritative_ || !lastLoadedPluginDescriptionValid_)
    {
        return;
    }

    const juce::String& plugName = lastLoadedPluginDescription_.name;
    if (!plugName.containsIgnoreCase("Groove Agent"))
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine("v2 capability persist: skipped reason=not_groove_agent");
        return;
    }

    const juce::File bundle(lastLoadedVst3OriginalPath_);
    if (lastLoadedVst3OriginalPath_.isEmpty() || !bundle.exists())
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine("v2 capability persist: skipped reason=no_bundle_path path=\""
                                                    + lastLoadedVst3OriginalPath_ + "\"");
        return;
    }

    juce::String derivationAttr = derivationLogReason;
    if (derivationLogReason == "duplicate_names_prefer_higher_primary_cluster")
    {
        derivationAttr = "cluster";
    }

    const int programIndexForXml
        = probe.selectedProgramIndex >= 0 ? probe.selectedProgramIndex : probe.activeProgramIndex;

    juce::String hashKey;
    for (int n = 24; n <= 51; ++n)
    {
        const auto it = rawPluginPitchNamesByNote_.find(n);
        if (it != rawPluginPitchNamesByNote_.end())
        {
            hashKey << n << "=" << it->second << "|";
        }
    }
    hashKey << "##";
    for (const int n : primaryPadDisplayActiveNotes_)
    {
        const auto it = rawPluginPitchNamesByNote_.find(n);
        hashKey << n << "=" << (it != rawPluginPitchNamesByNote_.end() ? it->second : juce::String{}) << "|";
    }
    hashKey << "##pi=" << programIndexForXml << "##d=" << derivationAttr;

    if (hashKey.isNotEmpty() && hashKey == lastGrooveDrumCapabilityPersistKey_)
    {
        return;
    }

    mini_daw::PluginCapabilities caps;
    mini_daw::RawPitchMapCapability rpm;
    rpm.source = "iUnitInfoProgramPitchName";
    rpm.programIndex = programIndexForXml;
    for (int n = 24; n <= 51; ++n)
    {
        const auto it = rawPluginPitchNamesByNote_.find(n);
        if (it != rawPluginPitchNamesByNote_.end() && it->second.isNotEmpty())
        {
            rpm.notes.push_back({n, it->second});
        }
    }
    caps.rawPitchMap = std::move(rpm);

    mini_daw::DrumNoteDisplayCapability dnd;
    dnd.derivation = derivationAttr;
    dnd.confidence = "high";
    for (const int n : primaryPadDisplayActiveNotes_)
    {
        mini_daw::DrumNoteDisplayActiveNote an;
        an.midi = n;
        const auto it = rawPluginPitchNamesByNote_.find(n);
        if (it != rawPluginPitchNamesByNote_.end())
        {
            an.rawName = it->second;
        }
        dnd.activeNotes.push_back(std::move(an));
    }
    caps.drumNoteDisplay = std::move(dnd);

    if ((!caps.rawPitchMap.has_value() || caps.rawPitchMap->notes.empty())
        && (!caps.drumNoteDisplay.has_value() || caps.drumNoteDisplay->activeNotes.empty()))
    {
        return;
    }

    bool wrote = false;
    try
    {
        wrote = mini_daw::mergeCapabilitiesIntoBundle(bundle, lastLoadedPluginDescription_, caps);
    }
    catch (...)
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine("v2 capability persist: merge threw (non-fatal)");
        return;
    }

    if (wrote)
    {
        lastGrooveDrumCapabilityPersistKey_ = hashKey;
    }
}

void ExperimentalInstrumentHost::runDrumNamePhaseCDiagnosticsIfEligible(
    const drum_name_diag::DrumNameRefreshPhase phase,
    const bool updateTransientNameCache,
    juce::AudioPluginInstance& liveInst)
{
    juce::ignoreUnused(phase);
    if (!drum_name_diag::kDrumNamesDiag || !updateTransientNameCache)
    {
        return;
    }

    writeDrumNameDiagnosticLogLine("drum-names: phaseC begin");

    std::set<int> metaCand;
    juce::String metaReason;
#if JUCE_PLUGINHOST_VST3 && (JUCE_WINDOWS || JUCE_MAC || JUCE_LINUX || JUCE_BSD)
    experimental_instrument_host_detail::runPhaseCMetadataDiagnostics(
        liveInst,
        rawPluginPitchNamesByNote_,
        metaCand,
        metaReason,
        [](const juce::String& ln) { writeDrumNameDiagnosticLogLine(ln); });
#else
    metaReason = "no_vst3_build";
#endif
    writeDrumNameDiagnosticLogLine("drum-names: phaseC metadata candidateActiveByMetadata="
                                   + formatIntSetForDiagLog(metaCand) + " reason=\"" + metaReason + "\"");

    if (!pluginDrumNameMapAuthoritative_)
    {
        writeDrumNameDiagnosticLogLine("drum-names: phaseC audio probe skipped reason=\"not authoritative\"");
        writeDrumNameDiagnosticLogLine("drum-names: phaseC end");
        return;
    }
    if (drumNamePhaseCAudioProbeShouldSkip_ != nullptr && drumNamePhaseCAudioProbeShouldSkip_())
    {
        writeDrumNameDiagnosticLogLine("drum-names: phaseC audio probe skipped reason=\"transport active\"");
        writeDrumNameDiagnosticLogLine("drum-names: phaseC end");
        return;
    }
    if (!lastLoadedPluginDescriptionValid_)
    {
        writeDrumNameDiagnosticLogLine("drum-names: phaseC audio probe skipped reason=\"no_plugin_description\"");
        writeDrumNameDiagnosticLogLine("drum-names: phaseC end");
        return;
    }

#if JUCE_PLUGINHOST_VST3 && (JUCE_WINDOWS || JUCE_MAC || JUCE_LINUX || JUCE_BSD)
    runDrumNamePhaseCAudioProbeIsolated(metaCand, liveInst);
#else
    writeDrumNameDiagnosticLogLine("drum-names: phaseC audio probe skipped reason=\"no_vst3_build\"");
#endif
    writeDrumNameDiagnosticLogLine("drum-names: phaseC end");
}

void ExperimentalInstrumentHost::runDrumNamePhaseCAudioProbeIsolated(
    const std::set<int>& metadataCandidateNotes,
    juce::AudioPluginInstance& liveInst)
{
#if !JUCE_PLUGINHOST_VST3 || (!JUCE_WINDOWS && !JUCE_MAC && !JUCE_LINUX && !JUCE_BSD)
    juce::ignoreUnused(metadataCandidateNotes, liveInst);
    return;
#else
    juce::String err;
    const double srU = effectiveSr(sampleRate_);
    const int bsU = effectiveBs(blockSize_);

    std::unique_ptr<juce::AudioPluginInstance> probe;
    try
    {
        probe = formatManager_.createPluginInstance(lastLoadedPluginDescription_, srU, bsU, err);
    }
    catch (const std::exception& e)
    {
        writeDrumNameDiagnosticLogLine("drum-names: phaseC audio probe FAILED reason=createPluginInstance_exception msg=\""
                                       + juce::String(e.what()) + "\"");
        return;
    }
    catch (...)
    {
        writeDrumNameDiagnosticLogLine("drum-names: phaseC audio probe FAILED reason=createPluginInstance_unknown");
        return;
    }

    if (probe == nullptr || probe->getPluginDescription().name.isEmpty())
    {
        writeDrumNameDiagnosticLogLine(
            "drum-names: phaseC audio probe FAILED reason=createPluginInstance err=\"" + err + "\"");
        return;
    }

    try
    {
        juce::MemoryBlock state;
        liveInst.getStateInformation(state);
        if (state.getSize() > 0)
        {
            probe->setStateInformation(state.getData(), (int)state.getSize());
        }
    }
    catch (const std::exception& e)
    {
        writeDrumNameDiagnosticLogLine("drum-names: phaseC audio probe FAILED reason=setState_exception msg=\""
                                       + juce::String(e.what()) + "\"");
        probe->releaseResources();
        return;
    }
    catch (...)
    {
        writeDrumNameDiagnosticLogLine("drum-names: phaseC audio probe FAILED reason=setState_unknown");
        probe->releaseResources();
        return;
    }

    if (!tryPrepareInstrumentLayout(*probe, sampleRate_, blockSize_))
    {
        writeDrumNameDiagnosticLogLine("drum-names: phaseC audio probe FAILED reason=prepare_layout");
        probe->releaseResources();
        return;
    }

    const int totalCh = juce::jmax(kStereoChannels, probe->getTotalNumOutputChannels());
    const int bs = effectiveBs(blockSize_);
    juce::AudioBuffer<float> buf(totalCh, bs);

    constexpr int kMidiChannel = 10;

    auto flushAllSound = [&](juce::AudioPluginInstance& p) {
        juce::MidiBuffer mb;
        mb.addEvent(juce::MidiMessage::allNotesOff(kMidiChannel), 0);
        mb.addEvent(juce::MidiMessage::allSoundOff(kMidiChannel), 0);
        for (int pass = 0; pass < 4; ++pass)
        {
            buf.setSize(totalCh, bs, false, false, true);
            buf.clear();
            p.processBlock(buf, mb);
            mb.clear();
        }
    };

    const int onSamples = (int)std::ceil(srU * 0.15);
    const int tailSamples = (int)std::ceil(srU * 0.20);
    const int latencyGate = (int)std::ceil(srU * 0.025);

    for (int note = 24; note <= 55; ++note)
    {
        flushAllSound(*probe);

        juce::String rawN;
        if (const auto it = rawPluginPitchNamesByNote_.find(note); it != rawPluginPitchNamesByNote_.end())
        {
            rawN = it->second;
        }
        const bool inFallbackPadRange = (note >= kFallbackPrimaryDrumPadDisplayMin
                                         && note <= kFallbackPrimaryDrumPadDisplayMax);
        const bool metaActive = metadataCandidateNotes.find(note) != metadataCandidateNotes.end();

        float peak = 0.f;
        double sumSq = 0.0;
        juce::int64 sampCount = 0;
        int firstNonzero = -1;
        int globalS = 0;
        bool sustainAfterNoteOff = false;
        float peakAfterNoteOffPhase = 0.f;

        auto measureRange = [&](const int nSamp) {
            for (int c = 0; c < buf.getNumChannels(); ++c)
            {
                const float* d = buf.getReadPointer(c);
                for (int i = 0; i < nSamp; ++i)
                {
                    const float x = d[i];
                    const float ax = std::abs(x);
                    peak = juce::jmax(peak, ax);
                    sumSq += (double)x * (double)x;
                    ++sampCount;
                    if (firstNonzero < 0 && ax > 1.0e-4f)
                    {
                        firstNonzero = globalS + i;
                    }
                }
            }
            globalS += nSamp;
        };

        auto processBlock = [&](const int nSamp, juce::MidiBuffer& mb, juce::AudioPluginInstance& p) {
            buf.setSize(totalCh, nSamp, false, false, true);
            buf.clear();
            p.processBlock(buf, mb);
            measureRange(nSamp);
        };

        bool noteOnSent = false;
        int pos = 0;
        while (pos < onSamples)
        {
            const int n = juce::jmin(bs, onSamples - pos);
            juce::MidiBuffer mb;
            if (!noteOnSent)
            {
                mb.addEvent(juce::MidiMessage::noteOn(kMidiChannel, note, (juce::uint8)100), 0);
                noteOnSent = true;
            }
            processBlock(n, mb, *probe);
            pos += n;
        }

        bool noteOffSent = false;
        int pos2 = 0;
        while (pos2 < tailSamples)
        {
            const int n = juce::jmin(bs, tailSamples - pos2);
            juce::MidiBuffer mb;
            if (!noteOffSent)
            {
                mb.addEvent(juce::MidiMessage::noteOff(kMidiChannel, note, (juce::uint8)0), 0);
                noteOffSent = true;
            }
            const float peakBefore = peak;
            juce::ignoreUnused(peakBefore);
            processBlock(n, mb, *probe);
            if (noteOffSent && pos2 == 0)
            {
                for (int c = 0; c < buf.getNumChannels(); ++c)
                {
                    const float* d = buf.getReadPointer(c);
                    for (int i = 0; i < n; ++i)
                    {
                        peakAfterNoteOffPhase = juce::jmax(peakAfterNoteOffPhase, std::abs(d[i]));
                    }
                }
            }
            pos2 += n;
        }
        sustainAfterNoteOff = peakAfterNoteOffPhase > 0.001f;

        flushAllSound(*probe);

        const float rms = sampCount > 0 ? (float)std::sqrt(sumSq / (double)juce::jmax<juce::int64>(1, sampCount)) : 0.f;
        const bool rawNonEmpty = rawN.isNotEmpty();
        const bool probeActive = rawNonEmpty && peak >= 0.005f && firstNonzero >= 0 && firstNonzero < latencyGate;
        const bool finalProposedDisplay = rawNonEmpty && (metaActive || probeActive);

        writeDrumNameDiagnosticLogLine(
            juce::String("drum-names: phaseC audio note=") + juce::String(note) + " raw=\"" + rawN
            + "\" inFallbackPadRange=" + juce::String(inFallbackPadRange ? "true" : "false") + " metadataActive="
            + juce::String(metaActive ? "true" : "false") + " peak=" + juce::String(peak, 6) + " rms="
            + juce::String(rms, 6) + " firstNonzeroSample=" + juce::String(firstNonzero) + " sustainAfterNoteOff="
            + juce::String(sustainAfterNoteOff ? "true" : "false") + " probeActive="
            + juce::String(probeActive ? "true" : "false") + " finalProposedDisplay="
            + juce::String(finalProposedDisplay ? "true" : "false"));
    }

    probe->releaseResources();
#endif
}

bool ExperimentalInstrumentHost::hasPluginDrumNameMapAvailable() const noexcept
{
    return pluginDrumNameMapAuthoritative_;
}

std::optional<juce::String> ExperimentalInstrumentHost::getPluginNoteNameIfAvailable(const int midiNote,
                                                                                     const int midiChannel) const
{
    juce::ignoreUnused(midiChannel);
    if (midiNote < 0 || midiNote > 127)
    {
        return std::nullopt;
    }
    auto o = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (o == nullptr || o->inst == nullptr || !o->layoutOk)
    {
        return std::nullopt;
    }
    const auto it = pluginPitchNamesByNote_.find(midiNote);
    if (it == pluginPitchNamesByNote_.end() || it->second.isEmpty())
    {
        return std::nullopt;
    }
    return it->second;
}

void ExperimentalInstrumentHost::openNativeEditor()
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    writeExperimentalInstrumentLogLine("native editor: open requested");

    auto owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner == nullptr || owner->inst == nullptr || !owner->layoutOk)
    {
        writeExperimentalInstrumentLogLine("native editor: FAILED (no instrument loaded)");
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon, "Experimental instrument", "No instrument is loaded.");
        return;
    }
    if (editorWindow_ != nullptr)
    {
        writeExperimentalInstrumentLogLine("native editor: toFront (already open)");
        editorWindow_->toFront(true);
        schedulePluginPitchNamesRefreshAfterNativeEditorOpened();
        return;
    }
    std::unique_ptr<juce::AudioProcessorEditor> ed(owner->inst->createEditor());
    if (ed == nullptr)
    {
        writeExperimentalInstrumentLogLine("native editor: FAILED (createEditor returned null)");
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Experimental instrument",
            "This plug-in did not provide an editor.");
        return;
    }
    editorWindow_ = std::make_unique<ExperimentalPluginEditorWindow>(*this, *owner->inst, std::move(ed));
    writeExperimentalInstrumentLogLine("native editor: open succeeded");
    schedulePluginPitchNamesRefreshAfterNativeEditorOpened();
}

void ExperimentalInstrumentHost::prepareForDevice(const double sampleRate, const int blockSize)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    writeExperimentalInstrumentLogLine(
        "prepareForDevice: sampleRate=" + juce::String(sampleRate, 6) + " blockSize=" + juce::String(blockSize));

    sampleRate_ = sampleRate;
    blockSize_ = juce::jmax(1, blockSize);
    if (midiIo_ != nullptr)
    {
        const juce::ScopedLock sl(midiIo_->midiLock);
        midiIo_->uiPendingMidi.clear();
    }
    rtBlockMidi_.clear();

    auto owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner != nullptr && owner->inst != nullptr && owner->layoutOk)
    {
        if (!tryPrepareInstrumentLayout(*owner->inst, sampleRate_, blockSize_))
        {
            writeExperimentalInstrumentLogLine(
                "prepareForDevice: FAILED bus negotiation - unloading instrument");
            juce::Logger::writeToLog(
                "[experimental-instrument] prepareForDevice failed — unloading instrument.");
            unloadInstrument();
            owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
        }
    }

    owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner != nullptr && owner->inst != nullptr && owner->layoutOk)
    {
        const int totalCh = owner->inst->getTotalNumOutputChannels();
        const int scratchCh = juce::jmax(kStereoChannels, totalCh);
        scratch_.setSize(scratchCh, blockSize_, false, true, true);
        scratchPtrs_.clear();
        scratchPtrs_.reserve((size_t)scratchCh);
        for (int c = 0; c < scratchCh; ++c)
        {
            scratchPtrs_.push_back(scratch_.getWritePointer(c));
        }
    }
    else
    {
        scratch_.setSize(kStereoChannels, blockSize_, false, true, true);
        scratchPtrs_.clear();
        scratchPtrs_.reserve((size_t)kStereoChannels);
        for (int c = 0; c < kStereoChannels; ++c)
        {
            scratchPtrs_.push_back(scratch_.getWritePointer(c));
        }
    }
}

void ExperimentalInstrumentHost::releaseResources()
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    writeExperimentalInstrumentLogLine("releaseResources: requested (device stopping)");

    auto owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner != nullptr && owner->inst != nullptr)
    {
        owner->inst->releaseResources();
        writeExperimentalInstrumentLogLine(
            "releaseResources: plugin releaseResources completed name=\"" + owner->inst->getName() + "\"");
    }
    else
    {
        writeExperimentalInstrumentLogLine("releaseResources: no loaded plugin instance");
    }
}

void ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs(float* const* outputChannelData,
                                                                         const int numOutputChannels,
                                                                         const int numSamples,
                                                                         float outputGain,
                                                                         float stereoPan) noexcept
{
    if (numSamples <= 0 || outputChannelData == nullptr || numOutputChannels <= 0)
    {
        rtDiag_skipBadIoArgs_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner == nullptr || owner->inst == nullptr || !owner->layoutOk)
    {
        rtDiag_skipNoOwnerBadLayout_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    juce::AudioPluginInstance& inst = *owner->inst;
    const int totalCh = inst.getTotalNumOutputChannels();

    const int scratchAllocatedSamples = scratch_.getNumSamples();
    const int scratchAllocatedChans = scratch_.getNumChannels();
    const bool scratchUndersized = scratchAllocatedSamples < numSamples || scratchAllocatedChans < kStereoChannels;
    if (totalCh < kStereoChannels || scratchUndersized)
    {
        if (scratchUndersized)
            rtDiag_skipScratchTooSmallForCallback_.fetch_add(1, std::memory_order_relaxed);
        else
            rtDiag_skipScratchLayout_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (midiIo_ == nullptr)
    {
        rtDiag_skipNoMidiIo_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    juce::MidiBuffer blockMidi;
    {
        const juce::ScopedLock sl(midiIo_->midiLock);
        blockMidi.addEvents(midiIo_->uiPendingMidi, 0, numSamples, 0);
        midiIo_->uiPendingMidi.clear();
    }
    blockMidi.addEvents(rtBlockMidi_, 0, numSamples, 0);
    rtBlockMidi_.clear();

    const int scratchCh = juce::jmin(scratchAllocatedChans, juce::jmax(kStereoChannels, totalCh));
    const int n = numSamples;
    jassert(n <= scratchAllocatedSamples);

    for (int c = 0; c < scratchCh; ++c)
    {
        if (float* p = scratch_.getWritePointer(c))
        {
            juce::FloatVectorOperations::clear(p, n);
        }
    }

    juce::AudioBuffer<float> view(
        scratchPtrs_.empty() ? nullptr : scratchPtrs_.data(), scratchCh, n);

    {
        juce::ScopedNoDenormals noDenormals;
        inst.processBlock(view, blockMidi);
    }

    const float* L = scratch_.getReadPointer(0);
    const float* R = scratch_.getReadPointer(1);

    float peak = 0.0f;
    if (L != nullptr && R != nullptr)
    {
        for (int i = 0; i < n; ++i)
        {
            peak = juce::jmax(peak, std::fabs(L[i]), std::fabs(R[i]));
        }
    }

    rtDiag_lastTotalOut_.store(totalCh, std::memory_order_relaxed);
    rtDiag_lastMainOut_.store(inst.getMainBusNumOutputChannels(), std::memory_order_relaxed);
    rtDiag_lastScratchChAllocated_.store(scratchAllocatedChans, std::memory_order_relaxed);
    rtDiag_lastProcessRun_.store(n, std::memory_order_relaxed);
    rtDiag_lastScratchPeakBits_.store(encodeScratchPeak(peak), std::memory_order_relaxed);
    constexpr float kEpsilon = 1.0e-12f;
    rtDiag_lastScratchAllZero_.store(peak <= kEpsilon ? 1 : 0, std::memory_order_relaxed);

    rtDiag_processOkBlocks_.fetch_add(1, std::memory_order_relaxed);

    const float g = juce::jmax(0.0f, outputGain);
    const float pL = trackPanLawGainLeft(stereoPan);
    const float pR = trackPanLawGainRight(stereoPan);
    addFirstStereoBusToDeviceOutputs(L, R, n, numOutputChannels, outputChannelData, g * pL, g * pR);
}
