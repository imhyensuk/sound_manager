add_library(smix_core STATIC
    src/Types.cpp
    src/FFT.cpp
    src/AudioAnalyzer.cpp
    src/PerceptualProfile.cpp
    src/PluginCatalog.cpp
    src/ParamSemantics.cpp
    src/ValueMapper.cpp
    src/ChainPlanner.cpp
    src/GainBalancer.cpp
    src/MixAction.cpp
    src/MixSession.cpp
    src/RecipeEngine.cpp
    src/LocalIntentInterpreter.cpp
    src/ClaudeMixAgent.cpp
    src/AutoMixer.cpp)
target_include_directories(smix_core PUBLIC include)
target_link_libraries(smix_core PUBLIC nlohmann_json::nlohmann_json)
if(NOT MSVC)
    target_compile_options(smix_core PRIVATE -Wall -Wextra)
endif()

if(SMIX_BUILD_TESTS)
    add_executable(smix_core_tests
        tests/main.cpp
        tests/AnalyzerTests.cpp
        tests/PlannerTests.cpp
        tests/ActionTests.cpp
        tests/IntentTests.cpp
        tests/AgentTests.cpp)
    target_link_libraries(smix_core_tests PRIVATE smix_core doctest::doctest)
    add_test(NAME smix_core_tests COMMAND smix_core_tests)
endif()
