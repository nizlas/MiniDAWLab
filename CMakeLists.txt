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

juce_generate_juce_header(MiniDAWLab)

target_include_directories(MiniDAWLab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_sources(MiniDAWLab PRIVATE
    src/app/MainAppWindow.cpp
    src/app/MainAppDialogs.cpp
    src/app/MainWindow.cpp
    src/app/ClipPasteboardController.cpp
    src/app/UndoRedoCoordinator.cpp
    src/app/AudioClipImportCoordinator.cpp
    src/app/MidiEditorPresenter.cpp
    src/app/InstrumentRuntimeCoordinator.cpp
    src/app/InstrumentTimelineRowCoordinator.cpp
    src/app/ProjectIoCoordinator.cpp
    src/app/RecordingCoordinator.cpp
    src/app/Vst3PluginPickerCoordinator.cpp
    src/MiniDAWLabApplication.cpp
    src/diagnostics/ExperimentalPlaybackRoutingLog.cpp
    src/instruments/InstrumentTrackController.cpp
    src/plugins/ExperimentalInstrumentHost.cpp
    src/plugins/PluginInsertHost.cpp
    src/plugins/PluginDiscovery.cpp
    src/plugins/Vst3ChildProcessScan.cpp
    src/plugins/PluginEditorWindows.cpp
    src/audio/AudioDeviceInfo.cpp
    src/audio/LatencySettingsStore.cpp
    src/Main.cpp
    src/transport/Transport.cpp
    src/domain/AudioClip.cpp
    src/domain/PlacedClip.cpp
    src/domain/Track.cpp
    src/domain/Session.cpp
    src/domain/SessionHistory.cpp
    src/diagnostics/UndoDiagnosticFileLog.cpp
    src/diagnostics/DrumNameDiagnosticFileLog.cpp
    src/domain/SessionSnapshot.cpp
    src/io/AudioFileLoader.cpp
    src/io/MonoWavFileWriter.cpp
    src/io/AudioWaveformCache.cpp
    src/io/ProjectFile.cpp
    src/io/ProjectAudioImport.cpp
    src/io/InstrumentMidiClipExport.cpp
    src/engine/CountInClickOutput.cpp
    src/playback/ExperimentalInstrumentPlaybackBridge.cpp
    src/engine/PlaybackEngine.cpp
    src/engine/RecorderService.cpp
    src/ui/ClipWaveformView.cpp
    src/ui/PlayheadOverlay.cpp
    src/ui/ForbiddenCursor.cpp
    src/ui/TimelineRulerView.cpp
    src/ui/TimelineLocatorPainter.cpp
    src/ui/LatencySettingsView.cpp
    src/ui/CollapsibleSideStrip.cpp
    src/ui/InspectorView.cpp
    src/ui/TimelineViewportModel.cpp
    src/ui/TrackLanesView.cpp
    src/ui/TrackHeaderView.cpp
    src/ui/experimental/ExperimentalMidiPatternPlayer.cpp
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

# JUCE juce_VST3PluginFormat.cpp + VST3 SDK headers: local `iid` triggers MSVC C4458 (hides base member).
# Safe to silence; module sources compile as part of this target.
if (MSVC)
    target_compile_options(MiniDAWLab PRIVATE /wd4458)
endif()
