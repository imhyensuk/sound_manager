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
    src/AutoMixer.cpp
    src/MixHistory.cpp
    src/MixAssistant.cpp
    src/Progress.cpp
    src/WavFile.cpp
    src/mem/MemoryRuntime.cpp
    src/modules/ModuleManager.cpp
    src/style/StyleProfile.cpp
    src/style/GenreProfile.cpp
    src/style/ProcessingLearner.cpp
    src/knowledge/PluginProfile.cpp
    src/knowledge/KnowledgeBase.cpp
    src/dialogue/Dialogue.cpp
    src/llm/IntentModel.cpp
    src/ear/EarModel.cpp
    src/dsp/Builtin.cpp
    src/dsp/Processors.cpp
    src/dsp/BuiltinKnowledge.cpp)
target_include_directories(smix_core PUBLIC include)
target_link_libraries(smix_core PUBLIC nlohmann_json::nlohmann_json)
if(SMIX_MEMOPRO_ENABLED)
    target_compile_definitions(smix_core PUBLIC SMIX_HAVE_MEMOPRO=1)
    target_link_libraries(smix_core PUBLIC memopro_c)
else()
    target_compile_definitions(smix_core PUBLIC SMIX_HAVE_MEMOPRO=0)
endif()
if(NOT MSVC)
    target_compile_options(smix_core PRIVATE -Wall -Wextra)
endif()

# The local language model lives in its own library so the core stays dependency-free.
add_library(smix_llm STATIC src/llm/LocalLlm.cpp)
target_link_libraries(smix_llm PUBLIC smix_core)
if(SMIX_WITH_LLAMA)
    target_link_libraries(smix_llm PRIVATE llama)
    target_compile_definitions(smix_llm PRIVATE SMIX_HAVE_LLAMA=1)
else()
    target_compile_definitions(smix_llm PRIVATE SMIX_HAVE_LLAMA=0)
endif()

# Offline tools for training and evaluating the models (training/README.md)
add_executable(smix_ear_features tools/smix_ear_features.cpp)
target_link_libraries(smix_ear_features PRIVATE smix_core)
add_executable(smix_ear_classify tools/smix_ear_classify.cpp)
target_link_libraries(smix_ear_classify PRIVATE smix_core)
add_executable(smix_learn_mix tools/smix_learn_mix.cpp)
target_link_libraries(smix_learn_mix PRIVATE smix_core)
add_executable(smix_export_prompts tools/smix_export_prompts.cpp)
target_link_libraries(smix_export_prompts PRIVATE smix_core)
add_executable(smix_intent_eval tools/smix_intent_eval.cpp)
target_link_libraries(smix_intent_eval PRIVATE smix_llm)

if(SMIX_BUILD_TESTS)
    add_executable(smix_core_tests
        tests/main.cpp
        tests/AnalyzerTests.cpp
        tests/PlannerTests.cpp
        tests/ActionTests.cpp
        tests/IntentTests.cpp
        tests/MemoryTests.cpp
        tests/HistoryTests.cpp
        tests/StyleTests.cpp
        tests/KnowledgeTests.cpp
        tests/AssistantTests.cpp
        tests/EarTests.cpp
        tests/LlmTests.cpp
        tests/GenreTests.cpp
        tests/DspTests.cpp
        tests/ProcessingTests.cpp)
    target_link_libraries(smix_core_tests PRIVATE smix_core smix_llm doctest::doctest)
    target_compile_definitions(smix_core_tests PRIVATE SMIX_TEST_TINY_LLM="${CMAKE_BINARY_DIR}/tiny-llm.gguf"
                                                       SMIX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME smix_core_tests COMMAND smix_core_tests)

    # Tiny random-weight GGUF (Qwen2 tokenizer) for the LLM test; needs Python + numpy.
    find_package(Python3 COMPONENTS Interpreter)
    if(SMIX_WITH_LLAMA AND Python3_FOUND)
        add_custom_command(
            OUTPUT "${CMAKE_BINARY_DIR}/tiny-llm.gguf"
            COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/training/tools/make_tiny_gguf.py" "${llama_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/tiny-llm.gguf"
            DEPENDS "${PROJECT_SOURCE_DIR}/training/tools/make_tiny_gguf.py"
            COMMENT "Generating the tiny test language model")
        add_custom_target(smix_tiny_llm DEPENDS "${CMAKE_BINARY_DIR}/tiny-llm.gguf")
        add_dependencies(smix_core_tests smix_tiny_llm)
    endif()
endif()
