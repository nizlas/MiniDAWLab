cmake_minimum_required(VERSION 3.22)

project(MiniDAWLab VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Non-owning IEditController* / IUnitInfo* on ExtensionsVisitor::VST3Client (see cmake/patches). Checkout first so
# `git apply` stays idempotent if those files were already patched in the FetchContent working tree.
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    PATCH_COMMAND git checkout HEAD --
        modules/juce_audio_processors/utilities/juce_ExtensionsVisitor.h
        modules/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp
        && git apply --ignore-whitespace
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/juce-8.0.4-vst3-extensions-visitor.patch"
    UPDATE_DISCONNECTED 1
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(MiniDAWLab
    PRODUCT_NAME "MiniDAWLab"
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "MiniDAWLab"
)

juce_add_binary_data(MiniDAWLabToolStripIcons
    HEADER_NAME MiniDAWLabToolStripBinaryData.h
    NAMESPACE mini_daw_tool_strip_icons
    SOURCES
        assets/ui/tools/pointer_tool.png
        assets/ui/tools/split_tool.png)

juce_generate_juce_header(MiniDAWLab)

target_include_directories(MiniDAWLab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_sources(MiniDAWLab PRIVATE
    src/app/MainAppWindow.cpp
    src/app/ProjectMainWindowBounds.cpp
    src/app/AddInstrumentTrackCoordinator.cpp
    src/app/ArrangementEventSelectionCoordinator.cpp
    src/app/MainAppDialogs.cpp
    src/app/MainMenuModel.cpp
    src/app/MainWindow.cpp
    src/app/ClipPasteboardController.cpp
    src/app/UndoRedoCoordinator.cpp
    src/app/AudioClipImportCoordinator.cpp
    src/app/InstrumentMidiImportCoordinator.cpp
    src/app/MidiEditorPresenter.cpp
    src/app/PluginHostUiBindings.cpp
    src/app/InstrumentMusicalUndoSnapshot.cpp
    src/app/InstrumentRuntimeCoordinator.cpp
    src/app/InstrumentTimelineRowCoordinator.cpp
    src/app/ProjectIoCoordinator.cpp
    src/app/RecordingCoordinator.cpp
    src/app/TrackLanesEditCoordinator.cpp
    src/app/TransportLayoutHelper.cpp
    src/app/TransportPlayPauseStopController.cpp
    src/app/Vst3PluginPickerCoordinator.cpp
    src/MiniDAWLabApplication.cpp
    src/diagnostics/ExperimentalPlaybackRoutingLog.cpp
    src/instruments/InstrumentTrackController.cpp
    src/plugins/ExperimentalInstrumentHost.cpp
    src/plugins/PluginInsertHost.cpp
    src/plugins/PluginDiscovery.cpp
    src/plugins/InstrumentCatalog.cpp
    src/plugins/Vst3ChildProcessScan.cpp
    src/plugins/PluginEditorWindows.cpp
    src/audio/AudioDeviceInfo.cpp
    src/audio/LatencySettingsStore.cpp
    src/Main.cpp
    src/transport/Transport.cpp
    src/domain/AudioClip.cpp
    src/domain/AudioMixdownProjectSettings.cpp
    src/domain/PlacedClip.cpp
    src/domain/Track.cpp
    src/domain/Session.cpp
    src/domain/SessionHistory.cpp
    src/diagnostics/UndoDiagnosticFileLog.cpp
    src/diagnostics/ProjectLoadDiagnosticLog.cpp
    src/diagnostics/PlaybackUiLoadLog.cpp
    src/diagnostics/StabilityDiagnosticLog.cpp
    src/diagnostics/CrashDumpHandler.cpp
    src/diagnostics/StabilityScenarioRunner.cpp
    # SPIKE-01 (P0/P1A validation spike; removable): hidden --spike01-state-capture panel.
    src/diagnostics/Spike01StateCapturePanel.cpp
    src/diagnostics/StabilityInvariants.cpp
    src/diagnostics/DrumNameDiagnosticFileLog.cpp
    src/domain/SessionSnapshot.cpp
    src/domain/SessionRouting.cpp
    src/io/AudioFileLoader.cpp
    src/io/MonoWavFileWriter.cpp
    src/io/AudioWaveformCache.cpp
    src/io/ProjectFile.cpp
    src/io/ProjectAudioImport.cpp
    src/io/InstrumentMidiClipExport.cpp
    src/engine/CountInClickOutput.cpp
    src/playback/ExperimentalInstrumentPlaybackBridge.cpp
    src/engine/PlaybackMixHelpers.cpp
    src/engine/RoutingPlanBuilder.cpp
    src/engine/PlaybackEngine.cpp
    src/app/AudioMixdownExporter.cpp
    src/app/AudioMixdownDialog.cpp
    src/engine/RecorderService.cpp
    src/ui/ClipWaveformView.cpp
    src/ui/EditToolIconStrip.cpp
    src/ui/ToolStripIconImages.cpp
    src/ui/PlayheadOverlay.cpp
    src/ui/UiPlayheadClock.cpp
    src/ui/ForbiddenCursor.cpp
    src/ui/TimelineRulerView.cpp
    src/ui/TimelineLocatorPainter.cpp
    src/ui/LatencySettingsView.cpp
    src/ui/CollapsibleSideStrip.cpp
    src/ui/InspectorPanControl.cpp
    src/ui/InspectorView.cpp
    src/ui/TimelineViewportModel.cpp
    src/ui/TrackLanesView.cpp
    src/ui/TrackHeaderView.cpp
    src/ui/experimental/ExperimentalMidiPatternPlayer.cpp
    src/ui/experimental/ExperimentalMidiPatternPlayerHostBinding.cpp
    src/ui/experimental/ExperimentalMidiImport.cpp
    src/ui/experimental/ExperimentalPianoRollView.cpp
    src/ui/experimental/ExperimentalMidiEditorWindow.cpp
)

target_compile_definitions(MiniDAWLab
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MiniDAWLab,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MiniDAWLab,JUCE_VERSION>"
)

target_link_libraries(MiniDAWLab
    PRIVATE
        MiniDAWLabToolStripIcons
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_gui_extra
        juce::juce_cryptography
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# -----------------------------------------------------------------------------
# MiniDAWSelftests (Phase B.1): deterministic Level-1 unit tests for the pure MIDI-editor logic
# headers (audition timing with a fake clock, title/status wording, octave labels, channel
# semantics). Console app; no audio device, no UI, no VST3. Run the exe: exit 0 = all green.
# -----------------------------------------------------------------------------
juce_add_console_app(MiniDAWSelftests PRODUCT_NAME "MiniDAWSelftests")
target_sources(MiniDAWSelftests PRIVATE
    tests/selftest/MiniDAWSelftestsMain.cpp
    src/io/InstrumentMidiClipExport.cpp
    src/io/ProjectFile.cpp
    src/diagnostics/ProjectLoadDiagnosticLog.cpp
    src/ui/experimental/ExperimentalMidiPatternPlayer.cpp
    # P1C (Portable Instruments): domain track/snapshot sources so the deterministic MIDI
    # dependency / proxy fingerprint tests run against the real SessionSnapshot, not mocks.
    src/domain/Track.cpp
    src/domain/PlacedClip.cpp
    src/domain/AudioClip.cpp
    src/domain/SessionSnapshot.cpp
    src/domain/SessionRouting.cpp)
target_include_directories(MiniDAWSelftests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(MiniDAWSelftests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(MiniDAWSelftests
    PRIVATE
        juce::juce_audio_basics
        # P1D: temporary-WAV write/validate in the ProxyRenderExecutor.h tests.
        juce::juce_audio_formats
        # P1C: proxy fingerprint hashing (juce::SHA256) in ProxyFingerprint.h tests.
        juce::juce_cryptography
        juce::juce_events
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# JUCE juce_VST3PluginFormat.cpp + VST3 SDK headers: local `iid` triggers MSVC C4458 (hides base member).
# Safe to silence; module sources compile as part of this target.
if (MSVC)
    target_compile_options(MiniDAWLab PRIVATE /wd4458)

    # Stability C1: produce a PDB in Release too so crash minidumps can be symbolized
    # (scripts/package-windows.ps1 archives it under dist\symbols). /OPT:REF and /OPT:ICF
    # keep the Release linker optimizations that /DEBUG would otherwise disable.
    target_compile_options(MiniDAWLab PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(MiniDAWLab PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

if (WIN32)
    # Stability C1: MiniDumpWriteDump in src/diagnostics/CrashDumpHandler.cpp.
    target_link_libraries(MiniDAWLab PRIVATE dbghelp)
endif()

# -----------------------------------------------------------------------------
# Optional MP3 encoder (packaging / dev convenience): copy locally supplied LAME beside MiniDAWLab.exe.
# Runtime lookup is unchanged: <executable_dir>/Tools/lame/lame.exe (see Tools/lame/README.txt).
# Source (not in git): external_tools/lame/lame.exe
# -----------------------------------------------------------------------------
set(MINIDAW_LAME_LOCAL_EXE "${CMAKE_CURRENT_SOURCE_DIR}/external_tools/lame/lame.exe")
if (EXISTS "${MINIDAW_LAME_LOCAL_EXE}")
    message(STATUS "Optional LAME encoder found at external_tools/lame/lame.exe (post-build will copy beside MiniDAWLab.exe)")
else()
    message(STATUS "Optional LAME encoder not found at external_tools/lame/lame.exe; MP3 export will be disabled unless lame.exe is copied manually.")
endif()

add_custom_command(
    TARGET MiniDAWLab
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            -DLAME_SRC="${MINIDAW_LAME_LOCAL_EXE}"
            "-DLAME_DST=$<TARGET_FILE_DIR:MiniDAWLab>/Tools/lame/lame.exe"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyOptionalLame.cmake"
    COMMENT "Optional: copy local LAME encoder for MP3 export (if external_tools/lame/lame.exe exists)"
    VERBATIM)
