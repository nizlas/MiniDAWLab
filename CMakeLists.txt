cmake_minimum_required(VERSION 3.22)

project(MiniDAWLab VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
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
    src/plugins/PluginInsertHost.cpp
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
    src/domain/SessionSnapshot.cpp
    src/io/AudioFileLoader.cpp
    src/io/MonoWavFileWriter.cpp
    src/io/ProjectAudioImport.cpp
    src/io/ProjectFile.cpp
    src/engine/CountInClickOutput.cpp
    src/engine/PlaybackEngine.cpp
    src/engine/RecorderService.cpp
    src/ui/ClipWaveformView.cpp
    src/ui/ForbiddenCursor.cpp
    src/ui/TimelineRulerView.cpp
    src/ui/LatencySettingsView.cpp
    src/ui/InspectorView.cpp
    src/ui/TimelineViewportModel.cpp
    src/ui/TrackLanesView.cpp
    src/ui/TrackHeaderView.cpp
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
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
