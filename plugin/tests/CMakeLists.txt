# Integration test: real VST3 fixtures hosted by the real Sound Manager processor.

foreach(kind EQ COMP)
    if(kind STREQUAL "EQ")
        set(fixture_name "SMX Test EQ")
        set(fixture_code SmEq)
    else()
        set(fixture_name "SMX Test Compressor")
        set(fixture_code SmCp)
    endif()

    juce_add_plugin(SmxFixture${kind}
        COMPANY_NAME "SMX Fixtures"
        BUNDLE_ID com.smx.fixture.${kind}
        PLUGIN_MANUFACTURER_CODE SmFx
        PLUGIN_CODE ${fixture_code}
        FORMATS VST3
        PRODUCT_NAME "${fixture_name}"
        VST3_CATEGORIES Fx
        COPY_PLUGIN_AFTER_BUILD FALSE)
    target_sources(SmxFixture${kind} PRIVATE FixturePlugins.cpp)
    target_compile_definitions(SmxFixture${kind} PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        SMX_FIXTURE_EQ=$<IF:$<STREQUAL:${kind},EQ>,1,0>)
    target_link_libraries(SmxFixture${kind} PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)
endforeach()

# Links against the plugin's shared-code library, so it must see the same JUCE headers and settings.
add_executable(SmxIntegrationTest IntegrationTest.cpp)
add_dependencies(SmxIntegrationTest SmxFixtureEQ_VST3 SmxFixtureCOMP_VST3 SoundManagerProfiler)
target_compile_definitions(SmxIntegrationTest PRIVATE
    $<TARGET_PROPERTY:SoundManager,COMPILE_DEFINITIONS>
    SMX_TEST_EQ_PATH="${CMAKE_CURRENT_BINARY_DIR}/SmxFixtureEQ_artefacts/$<CONFIG>/VST3/SMX Test EQ.vst3"
    SMX_TEST_COMP_PATH="${CMAKE_CURRENT_BINARY_DIR}/SmxFixtureCOMP_artefacts/$<CONFIG>/VST3/SMX Test Compressor.vst3"
    SMX_PROFILER_PATH="$<TARGET_FILE:SoundManagerProfiler>")
target_include_directories(SmxIntegrationTest PRIVATE $<TARGET_PROPERTY:SoundManager,INCLUDE_DIRECTORIES>)
target_link_libraries(SmxIntegrationTest PRIVATE SoundManager smix_core smix_llm)
if(APPLE)
    target_link_libraries(SmxIntegrationTest PRIVATE "-framework CoreAudioKit" "-framework AudioUnit" "-framework AudioToolbox")
endif()
add_test(NAME smix_integration COMMAND SmxIntegrationTest)
