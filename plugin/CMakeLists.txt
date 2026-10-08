# Sound Manager AI - JUCE plugin target
#
#   VST3       : Cubase, FL Studio, Ableton Live, Audacity, Reaper, Studio One, Bitwig ...
#   AU         : Logic Pro, GarageBand (macOS only)
#   AAX        : Pro Tools (needs the Avid AAX SDK + PACE signing, see docs/BUILDING.md)
#   LV2        : Audacity / Ardour on Linux
#   Standalone : quick testing without a DAW

set(SMIX_AAX_SDK_PATH "" CACHE PATH "Path to the Avid AAX SDK (enables the AAX format)")

set(SMIX_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND SMIX_FORMATS AU)
endif()
if(UNIX AND NOT APPLE)
    list(APPEND SMIX_FORMATS LV2)
endif()
if(SMIX_AAX_SDK_PATH)
    juce_set_aax_sdk_path("${SMIX_AAX_SDK_PATH}")
    list(APPEND SMIX_FORMATS AAX)
endif()

juce_add_plugin(SoundManager
    COMPANY_NAME "Sound Manager"
    BUNDLE_ID "com.soundmanager.ai"
    PLUGIN_MANUFACTURER_CODE Smgr
    PLUGIN_CODE Smx1
    FORMATS ${SMIX_FORMATS}
    PRODUCT_NAME "Sound Manager AI"
    DESCRIPTION "AI mixing assistant that drives your own plugins"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    NEEDS_CURL TRUE
    VST3_CATEGORIES Fx Analyzer
    AU_MAIN_TYPE kAudioUnitType_Effect
    AAX_CATEGORY AAX_ePlugInCategory_Dynamics
    LV2URI "https://github.com/imhyensuk/sound_manager"
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(SoundManager PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/HostedChain.cpp
    Source/PluginLibrary.cpp
    Source/SessionHub.cpp
    Source/AgentWorker.cpp
    Source/JuceHttpTransport.cpp)

target_compile_definitions(SoundManager PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # Hosting: the plugin loads the user's own plugins inside itself.
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0)

target_link_libraries(SoundManager
    PRIVATE
        smix_core
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

option(SMIX_BUILD_INTEGRATION_TEST "Build VST3 fixtures and the hosting integration test" OFF)
if(SMIX_BUILD_INTEGRATION_TEST)
    add_subdirectory(tests)
endif()
